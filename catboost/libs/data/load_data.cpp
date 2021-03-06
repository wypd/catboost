#include "load_data.h"
#include "load_helpers.h"

#include <catboost/libs/helpers/mem_usage.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/model/split.h>

#include <library/grid_creator/binarization.h>
#include <library/threading/local_executor/local_executor.h>

#include <util/digest/city.h>
#include <util/generic/set.h>
#include <util/generic/map.h>
#include <util/generic/hash_set.h>
#include <util/string/split.h>
#include <util/system/fs.h>
#include <util/system/event.h>
#include <util/stream/file.h>
#include <util/system/spinlock.h>

class TPoolBuilder: public IPoolBuilder {
public:
    explicit TPoolBuilder(TPool* pool)
        : Pool(pool)
    {
    }

    void Start(const TPoolColumnsMetaInfo& poolMetaInfo) override {
        Pool->Docs.clear();
        FactorCount = poolMetaInfo.FactorCount;
        BaselineCount = poolMetaInfo.BaselineCount;

        if (poolMetaInfo.HasQueryIds) {
            MATRIXNET_WARNING_LOG << "We don't support query ids currently" << Endl;
        }

        Pool->CatFeatures = poolMetaInfo.CatFeatureIds;
    }

    void StartNextBlock(ui32 blockSize) override {
        Cursor = (ui32)Pool->Docs.size();
        Pool->Docs.resize(Pool->Docs.size() + blockSize);
        for (ui32 i = Cursor; i < Pool->Docs.size(); ++i) {
            Pool->Docs[i].Factors.resize(FactorCount);
            Pool->Docs[i].Baseline.resize(BaselineCount);
        }
    }

    void AddCatFeature(ui32 localIdx, ui32 featureId, const TStringBuf& feature) override {
        int hashVal = CalcCatFeatureHash(feature);
        auto& curPart = LockedHashMapParts[hashVal & 0xff];
        with_lock (curPart.Lock) {
            if (!curPart.CatFeatureHashes.has(hashVal)) {
                curPart.CatFeatureHashes[hashVal] = feature;
            }
        }
        AddFloatFeature(localIdx, featureId, ConvertCatFeatureHashToFloat(hashVal));
    }

    void AddFloatFeature(ui32 localIdx, ui32 featureId, float feature) override {
        GetLine(localIdx).Factors[featureId] = feature;
    }

    void AddTarget(ui32 localIdx, float value) override {
        GetLine(localIdx).Target = value;
    }

    void AddWeight(ui32 localIdx, float value) override {
        GetLine(localIdx).Weight = value;
    }

    void AddQueryId(ui32 localIdx, const TStringBuf& queryId) override {
        Y_UNUSED(localIdx);
        Y_UNUSED(queryId);
    }

    void AddBaseline(ui32 localIdx, ui32 offset, double value) override {
        GetLine(localIdx).Baseline[offset] = value;
    }

    void AddDocId(ui32 localIdx, const TStringBuf& value) override {
        GetLine(localIdx).Id = value;
    }

    void SetFeatureIds(const yvector<TString>& featureIds) override {
        Y_ENSURE(featureIds.size() == FactorCount, "Error: feature ids size should be equal to factor count");
        Pool->FeatureId = featureIds;
    }

    void SetPairs(const yvector<TPair>& pairs) override {
        Pool->Pairs = pairs;
    }

    int GetDocCount() const override {
        return Pool->Docs.ysize();
    }

    void Finish() override {
        if (!Pool->Docs.empty()) {
            for (const auto& part : LockedHashMapParts) {
                Pool->CatFeaturesHashToString.insert(part.CatFeatureHashes.begin(), part.CatFeatureHashes.end());
            }
            MATRIXNET_INFO_LOG << "Doc info sizes: " << Pool->Docs.size() << " " << FactorCount << Endl;
        } else {
            MATRIXNET_ERROR_LOG << "No doc info loaded" << Endl;
        }
    }

private:
    struct TLockedHashPart {
        TAdaptiveLock Lock;
        yhash<int, TString> CatFeatureHashes;
    };
    TDocInfo& GetLine(ui32 localIdx) {
        return Pool->Docs[Cursor + localIdx];
    }
    TPool* Pool;
    ui32 Cursor = 0;
    ui32 FactorCount = 0;
    ui32 BaselineCount = 0;
    std::array<TLockedHashPart, 256> LockedHashMapParts;
};

TTargetConverter::TTargetConverter(const yvector<TString>& classNames)
    : ClassNames(classNames)
{
}

float TTargetConverter::operator()(const TString& word) const {
    if (ClassNames.empty()) {
        return FromString<float>(word);
    }

    for (int classIndex = 0; classIndex < ClassNames.ysize(); ++classIndex) {
        if (ClassNames[classIndex] == word) {
            return classIndex;
        }
    }

    CB_ENSURE(false, "Unknown class name: " + word);
    return UNDEFINED_CLASS;
}

static yvector<int> GetCategFeatures(const yvector<TColumn>& columns) {
    Y_ASSERT(!columns.empty());
    yvector<int> categFeatures;
    int featureId = 0;
    for (const TColumn& column : columns) {
        switch (column.Type) {
            case EColumn::Categ:
                categFeatures.push_back(featureId);
                ++featureId;
                break;
            case EColumn::Num:
                ++featureId;
                break;
            case EColumn::Auxiliary:
            case EColumn::Target:
            case EColumn::Baseline:
            case EColumn::Weight:
            case EColumn::DocId:
            case EColumn::QueryId:
                break;
            default:
                CB_ENSURE(false, "this column type is not supported");
        }
    }
    return categFeatures;
}

static bool IsNan(const TStringBuf& s) {
    return s == "nan" || s == "NaN" || s == "NAN";
}

void StartBuilder(const yvector<TString>& featureIds, const TPoolColumnsMetaInfo& poolMetaInfo,
                bool hasHeader, IPoolBuilder* poolBuilder) {
    poolBuilder->Start(poolMetaInfo);
    if (hasHeader) {
        poolBuilder->SetFeatureIds(featureIds);
    }
}

void FinalizeBuilder(const yvector<TColumn>& columnsDescription, const TString& pairsFile, IPoolBuilder* poolBuilder) {
    DumpMemUsage("After data read");
    if (!AllOf(columnsDescription.begin(), columnsDescription.end(), [](const TColumn& column) {
            return column.Id.empty();
        })) {
        yvector<TString> featureIds;
        for (auto column : columnsDescription) {
            if (column.Type == EColumn::Categ || column.Type == EColumn::Num) {
                featureIds.push_back(column.Id);
            }
        }
        poolBuilder->SetFeatureIds(featureIds);
    }
    poolBuilder->Finish();
    if (!pairsFile.empty()) {
         yvector<TPair> pairs = ReadPairs(pairsFile, poolBuilder->GetDocCount());
         poolBuilder->SetPairs(pairs);
     }
}

TPoolReader::TPoolReader(const TString& cdFile, const TString& poolFile, const TString& pairsFile, int threadCount, char fieldDelimiter,
                         bool hasHeader, const yvector<TString>& classNames, IPoolBuilder* poolBuilder, int blockSize)
    : PairsFile(pairsFile)
    , ThreadCount(threadCount)
    , LinesRead(0)
    , FieldDelimiter(fieldDelimiter)
    , HasHeader(hasHeader)
    , ConvertTarget(classNames)
    , BlockSize(blockSize)
    , Reader(poolFile.c_str())
    , PoolBuilder(*poolBuilder)
{
    CB_ENSURE(threadCount > 0);
    CB_ENSURE(NFs::Exists(TString(poolFile)), "pool file is not found " + TString(poolFile));
    const int columnsCount = ReadColumnsCount(poolFile, FieldDelimiter);

    if (!cdFile.empty()) {
        ColumnsDescription = ReadCD(cdFile, columnsCount);
    } else {
        ColumnsDescription.assign(columnsCount, TColumn{EColumn::Num, TString()});
        ColumnsDescription[0].Type = EColumn::Target;
    }
    const int weightColumns = (const int)CountIf(ColumnsDescription.begin(),
                                                 ColumnsDescription.end(),
                                                 [](const auto x) -> bool {
                                                     return x.Type == EColumn::Weight;
                                                 });

    CB_ENSURE(weightColumns <= 1, "Too many weight columns");
    PoolMetaInfo.HasWeights = (bool)weightColumns;

    PoolMetaInfo.BaselineCount = (ui32)CountIf(ColumnsDescription.begin(),
                                               ColumnsDescription.end(),
                                               [](const auto x) -> bool {
                                                   return x.Type == EColumn::Baseline;
                                               });

    const int targetColumns = (const int)CountIf(ColumnsDescription.begin(),
                                                 ColumnsDescription.end(),
                                                 [](const auto x) -> bool {
                                                     return x.Type == EColumn::Target;
                                                 });

    CB_ENSURE(targetColumns <= 1, "Too many target columns");

    const int docIdColumns = (const int)CountIf(ColumnsDescription.begin(),
                                                ColumnsDescription.end(),
                                                [](const auto x) -> bool {
                                                    return x.Type == EColumn::DocId;
                                                });

    CB_ENSURE(docIdColumns <= 1, "Too many DocId columns");
    PoolMetaInfo.HasDocIds = (bool)docIdColumns;

    const int queryIdColumns = (const int)CountIf(ColumnsDescription.begin(),
                                                  ColumnsDescription.end(),
                                                  [](const auto x) -> bool {
                                                      return x.Type == EColumn::QueryId;
                                                  });

    CB_ENSURE(queryIdColumns <= 1, "Too many queryId columns");
    PoolMetaInfo.HasQueryIds = (bool)queryIdColumns;

    PoolMetaInfo.FactorCount = (const ui32)CountIf(ColumnsDescription.begin(),
                                                   ColumnsDescription.end(),
                                                   [](const auto x) -> bool {
                                                       return IsFactorColumn(x.Type);
                                                   });

    CB_ENSURE(PoolMetaInfo.FactorCount > 0, "Pool should have at least one factor");
    PoolMetaInfo.CatFeatureIds = GetCategFeatures(ColumnsDescription);

    LocalExecutor.RunAdditionalThreads(ThreadCount - 1);

    if (HasHeader) {
        TString line;
        Reader.ReadLine(line);

        yvector<TStringBuf> words;
        SplitRangeTo<const char, yvector<TStringBuf>>(~line, ~line + line.size(), FieldDelimiter, &words);
        CB_ENSURE(words.ysize() == ColumnsDescription.ysize(), "wrong columns number in pool header");

        for (int i = 0; i < words.ysize(); ++i) {
            if (ColumnsDescription[i].Type == EColumn::Categ || ColumnsDescription[i].Type == EColumn::Num) {
                TString Id;
                TryFromString<TString>(words[i], Id);
                FeatureIds.push_back(Id);
            }
        }
    }
    ReadBlockAsync();
}

void TPoolReader::ReadBlockAsync() {
    auto readLineBufferLambda = [&](int) {
        TString bufReadLine;
        ReadBuffer.clear();
        while (ReadBuffer.size() < BlockSize && Reader.ReadLine(bufReadLine)) {
            ReadBuffer.push_back(bufReadLine);
            bufReadLine.clear();
        }
        BlockReadCompletedEvent.Signal();
    };
    if (LocalExecutor.GetThreadCount() > 0) {
        LocalExecutor.Exec(readLineBufferLambda, 0, NPar::TLocalExecutor::HIGH_PRIORITY);
    } else {
        readLineBufferLambda(0);
    }
}

bool TPoolReader::ReadBlock() {
    BlockReadCompletedEvent.Wait();
    ReadBuffer.swap(ParseBuffer);
    if (!!ParseBuffer) {
        ReadBlockAsync();
    }
    return !!ParseBuffer;
}

void TPoolReader::ProcessBlock() {
    PoolBuilder.StartNextBlock((ui32)ParseBuffer.size());
    auto parseFeaturesInBlock = [&](int lineIdx) {
        yvector<TStringBuf> words;
        const auto& line = ParseBuffer[lineIdx];
        SplitRangeTo<const char, yvector<TStringBuf>>(~line, ~line + line.size(), FieldDelimiter, &words);
        CB_ENSURE(words.ysize() == ColumnsDescription.ysize(), "wrong columns number in pool line " <<
                  lineIdx + 1 << ": expected " << ColumnsDescription.ysize() << ", found " << words.ysize());

        ui32 featureId = 0;
        ui32 baselineIdx = 0;
        for (int i = 0; i < words.ysize(); ++i) {
            switch (ColumnsDescription[i].Type) {
                case EColumn::Categ: {
                    PoolBuilder.AddCatFeature(lineIdx, featureId, words[i]);
                    ++featureId;
                    break;
                }
                case EColumn::Num: {
                    float val;
                    CB_ENSURE(words[i] != "", "empty values not supported");
                    if (!TryFromString<float>(words[i], val)) {
                        if (IsNan(words[i])) {
                            val = std::numeric_limits<float>::quiet_NaN();
                        } else {
                            CB_ENSURE(false, "Factor " << words[i] << " in column " << i + 1 << " and row " << LinesRead + lineIdx + 1 <<
                                      " is declared as numeric and cannot be parsed as float. Try correcting column description file.");
                        }
                    }
                    PoolBuilder.AddFloatFeature(lineIdx, featureId, val);
                    ++featureId;
                    break;
                }
                case EColumn::Target: {
                    CB_ENSURE(words[i] != "", "empty values not supported for target. Target should be float.");
                    PoolBuilder.AddTarget(lineIdx, ConvertTarget(FromString<TString>(words[i])));
                    break;
                }
                case EColumn::Weight: {
                    CB_ENSURE(words[i] != "", "empty values not supported for weight");
                    PoolBuilder.AddWeight(lineIdx, FromString<float>(words[i]));
                    break;
                }
                case EColumn::Auxiliary: {
                    break;
                }
                case EColumn::QueryId: {
                    PoolBuilder.AddQueryId(lineIdx, words[i]);
                    break;
                }
                case EColumn::Baseline: {
                    CB_ENSURE(words[i] != "", "empty values not supported for baseline");
                    PoolBuilder.AddBaseline(lineIdx, baselineIdx, FromString<double>(words[i]));
                    ++baselineIdx;
                    break;
                }
                case EColumn::DocId: {
                    CB_ENSURE(words[i] != "", "empty values not supported for doc id");
                    PoolBuilder.AddDocId(lineIdx, words[i]);
                    break;
                }
                default: {
                    CB_ENSURE(false, "wrong column type");
                }
            }
        }
    };
    LocalExecutor.ExecRange(parseFeaturesInBlock, 0, ParseBuffer.ysize(), NPar::TLocalExecutor::WAIT_COMPLETE);
    LinesRead += ParseBuffer.ysize();
}

THolder<IPoolBuilder> InitBuilder(TPool* pool) {
    return new TPoolBuilder(pool);
}

void ReadPool(const TString& cdFile,
              const TString& poolFile,
              const TString& pairsFile,
              int threadCount,
              bool verbose,
              char fieldDelimiter,
              bool hasHeader,
              const yvector<TString>& classNames,
              TPool* pool) {
    TPoolBuilder builder(pool);
    ReadPool(cdFile, poolFile, pairsFile, threadCount, verbose, fieldDelimiter, hasHeader, classNames, &builder);
}

void ReadPool(const TString& cdFile,
              const TString& poolFile,
              const TString& pairsFile,
              int threadCount,
              bool verbose,
              char fieldDelimiter,
              bool hasHeader,
              const yvector<TString>& classNames,
              IPoolBuilder* poolBuilder) {
    if (verbose) {
        SetVerboseLogingMode();
    } else {
        SetSilentLogingMode();
    }
    TPoolReader poolReader(cdFile, poolFile, pairsFile, threadCount, fieldDelimiter, hasHeader, classNames, poolBuilder, 10000);
    StartBuilder(poolReader.FeatureIds, poolReader.PoolMetaInfo, hasHeader, poolBuilder);
    while (poolReader.ReadBlock()) {
         poolReader.ProcessBlock();
    }
    FinalizeBuilder(poolReader.ColumnsDescription, poolReader.PairsFile, poolBuilder);
    SetVerboseLogingMode();
}

void ReadPool(const TString& cdFile,
              const TString& poolFile,
              const TString& pairsFile,
              int threadCount,
              bool verbose,
              IPoolBuilder& poolBuilder) {
    yvector<TString> noNames;
    ReadPool(cdFile, poolFile, pairsFile, threadCount, verbose, '\t', false, noNames, &poolBuilder);
}

