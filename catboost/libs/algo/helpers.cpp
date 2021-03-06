#include "helpers.h"

#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/logging/logging.h>

#include <util/generic/algorithm.h>
#include <util/system/mem_info.h>

void GenerateBorders(const yvector<TDocInfo>& docInfos, TLearnContext* ctx, yvector<yvector<float>>* borders, yvector<bool>* hasNans) {

    const yhash_set<int>& categFeatures = ctx->CatFeatures;
    const int borderCount = ctx->Params.BorderCount;
    const EBorderSelectionType borderType = ctx->Params.FeatureBorderType;

    size_t reasonCount = docInfos[0].Factors.size() - categFeatures.size();
    borders->resize(reasonCount);
    hasNans->resize(reasonCount);
    if (reasonCount == 0) {
        return;
    }

    yvector<int> floatIndexes;
    for (int i = 0; i < docInfos[0].Factors.ysize(); ++i) {
        if (!categFeatures.has(i)) {
            floatIndexes.push_back(i);
        }
    }
    // Estimate how many threads can generate borders
    const size_t bytes1M = 1024 * 1024, bytesThreadStack = 2 * bytes1M;
    const size_t bytesUsed = NMemInfo::GetMemInfo().RSS;
    const size_t bytesBestSplit = (sizeof(float) + (borderCount - 1) * sizeof(size_t) + 2 * sizeof(double) + 2 * sizeof(size_t) + 2 * sizeof(double)) * docInfos.ysize();
    const size_t bytesGenerateBorders = sizeof(float) * docInfos.ysize();
    const size_t bytesRequiredPerThread = bytesThreadStack + bytesGenerateBorders + bytesBestSplit;
    const size_t threadCount = Min(reasonCount, (ctx->Params.UsedRAMLimit - bytesUsed) / bytesRequiredPerThread);
    CB_ENSURE(ctx->Params.UsedRAMLimit >= bytesUsed && threadCount > 0, "CatBoost needs " << (bytesUsed + bytesRequiredPerThread) / bytes1M + 1 << " Mb of memory to generate borders");

    TAtomic taskFailedBecauseOfNans = 0;
    auto calcOneFeatureBorder = [&](int idx) {
        const auto floatFeatureIdx = floatIndexes[idx];

        yvector<float> vals;

        (*hasNans)[idx] = false;
        for (int i = 0; i < docInfos.ysize(); ++i) {
            if (!IsNan(docInfos[i].Factors[floatFeatureIdx])) {
                vals.push_back(docInfos[i].Factors[floatFeatureIdx]);
            } else {
                (*hasNans)[idx] = true;
            }
        }
        Sort(vals.begin(), vals.end());

        yhash_set<float> borderSet = BestSplit(vals, borderCount, borderType);
        yvector<float> bordersBlock(borderSet.begin(), borderSet.end());
        Sort(bordersBlock.begin(), bordersBlock.end());
        if ((*hasNans)[idx]) {
            if (ctx->Params.NanMode == ENanMode::Min) {
                bordersBlock.insert(bordersBlock.begin(), std::numeric_limits<float>::lowest());
            } else if (ctx->Params.NanMode == ENanMode::Max) {
                bordersBlock.push_back(std::numeric_limits<float>::max());
            } else {
                Y_ASSERT(ctx->Params.NanMode == ENanMode::Forbidden);
                taskFailedBecauseOfNans = 1;
            }
        }
        (*borders)[idx].swap(bordersBlock);
    };
    size_t nReason = 0;
    for (; nReason + threadCount <= reasonCount; nReason += threadCount) {
        ctx->LocalExecutor.ExecRange(calcOneFeatureBorder, nReason, nReason + threadCount, NPar::TLocalExecutor::WAIT_COMPLETE);
        CB_ENSURE(taskFailedBecauseOfNans == 0, "There are nan factors and nan values for float features are not allowed. Set nan_mode != Forbidden.");
    }
    for (; nReason < reasonCount; ++nReason) {
        calcOneFeatureBorder(nReason);
    }

    MATRIXNET_INFO_LOG << "Borders for float features generated" << Endl;
}

void ApplyPermutation(const yvector<size_t>& permutation, TPool* pool) {
    Y_VERIFY(permutation.ysize() == pool->Docs.ysize());

    yvector<size_t> toIndices(permutation);
    for (size_t i = 0; i < permutation.size(); ++i) {
        while (toIndices[i] != i) {
            auto destinationIndex = toIndices[i];
            DoSwap(pool->Docs[i], pool->Docs[destinationIndex]);
            DoSwap(toIndices[i], toIndices[destinationIndex]);
        }
    }

    for (auto& pair : pool->Pairs) {
        pair.WinnerId = permutation[pair.WinnerId];
        pair.LoserId = permutation[pair.LoserId];
    }
}

yvector<size_t> InvertPermutation(const yvector<size_t>& permutation) {
    yvector<size_t> result(permutation.size());
    for (size_t i = 0; i < permutation.size(); ++i) {
        result[permutation[i]] = i;
    }
    return result;
}

int GetClassesCount(const yvector<float>& target, int classesCount) {
    int maxClass = static_cast<int>(*MaxElement(target.begin(), target.end()));
    if (classesCount == 0) { // classesCount not set
        return maxClass + 1;
    } else {
        CB_ENSURE(maxClass < classesCount, "if classes-count is specified then each target label should be in range 0,..,classes_count-1");
        return classesCount;
    }
}
