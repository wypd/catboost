#pragma once

#include <catboost/cuda/data/pool_proto/pool.pb.h>
#include <library/protobuf/protofile/protofile.h>
#include <util/stream/file.h>
#include <util/generic/vector.h>

template <class TProtoMessage>
inline void ReadMessage(TIFStream& input, TProtoMessage& message) {
    message.Clear();
    NFastTier::TBinaryProtoReader<TProtoMessage> messageReader;
    messageReader.Open(&input);
    messageReader.GetNext(message);
}

inline void ReadUnsignedIntColumn(TIFStream& input, yvector<ui32>& dst) {
    NCompressedPool::TUnsignedIntegerColumn column;
    ReadMessage(input, column);
    dst.resize((unsigned long)column.GetValues().size());
    std::copy(column.GetValues().begin(), column.GetValues().end(), dst.begin());
}

inline void ReadIntColumn(TIFStream& input, yvector<i32>& dst) {
    NCompressedPool::TIntegerColumn column;
    ReadMessage(input, column);
    dst.resize((unsigned long)column.GetValues().size());
    std::copy(column.GetValues().begin(), column.GetValues().end(), dst.begin());
}

inline void ReadFloatColumn(TIFStream& input, yvector<float>& dst) {
    NCompressedPool::TFloatColumn column;
    ReadMessage(input, column);
    dst.resize((unsigned long)column.GetValues().size());
    std::copy(column.GetValues().begin(), column.GetValues().end(), dst.begin());
}
