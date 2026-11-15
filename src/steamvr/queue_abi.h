#pragma once
#include <cstdint>

namespace ct::steamvr {

// IVRBlockQueue_005 ABI from Valve's pinned openvr_capi.h and stock driver calls.
class BlockQueue {
  public:
    virtual int Create(uint64_t *, const char *, uint32_t, uint32_t, uint32_t, uint32_t) = 0;
    virtual int Connect(uint64_t *, const char *) = 0;
    virtual int Destroy(uint64_t) = 0;
    virtual int AcquireWriteOnlyBlock(uint64_t, uint64_t *, void **) = 0;
    virtual int ReleaseWriteOnlyBlock(uint64_t, uint64_t) = 0;
    virtual int WaitAndAcquireReadOnlyBlock(uint64_t, uint64_t *, void **, int, uint32_t) = 0;
    virtual int AcquireReadOnlyBlock(uint64_t, uint64_t *, void **, int) = 0;
    virtual int ReleaseReadOnlyBlock(uint64_t, uint64_t) = 0;
    virtual int QueueHasReader(uint64_t, bool *) = 0;
};

struct PathWrite {
    uint64_t path;
    int type, set_error;
    void *buffer;
    uint32_t size, tag;
    int error;
    const char *name;
    bool post_events, changed;
};
static_assert(sizeof(PathWrite) == 56);

class Paths {
  public:
    virtual int ReadPathBatch(uint64_t, void *, uint32_t) = 0;
    virtual int WritePathBatch(uint64_t, PathWrite *, uint32_t) = 0;
    virtual int StringToHandle(uint64_t *, const char *) = 0;
};

} // namespace ct::steamvr
