#include "platform/linux/resource.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <json-c/json.h>
#include <linux/dma-buf.h>
#include <linux/media.h>
#include <linux/udmabuf.h>
#include <linux/videodev2.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unordered_map>
#include <vector>

namespace {
using ct::FileDescriptor;
using Ioctl = int (*)(int, unsigned long, ...);
Ioctl ioctl_next() {
    static auto next = reinterpret_cast<Ioctl>(dlsym(RTLD_NEXT, "ioctl"));
    return next;
}

constexpr auto Capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr unsigned MinBuffers = 8;
constexpr double Tolerance = .001;
constexpr double TimerHz = 19200000.;
constexpr size_t FrontViews = 2;
constexpr size_t ViewCount = 4;

struct Camera {
    const char *name;
    const char *video_name;
    unsigned width, height;
    size_t dma_size;
    std::string serial;
    int device = -1;
    double last_sof = 0;
    unsigned stride = 0;
};

struct Backing {
    FileDescriptor file;
    ino_t inode;
    uint64_t offset;
};

struct View {
    FileDescriptor file, dma;
    ino_t inode = 0;
    uint64_t offset = 0;
    double time = 0;
    int device = -1;
    v4l2_buffer queue{};
    std::array<v4l2_plane, 2> planes{};
};

// Some Frames have XRService set the SLAM nodes to NV12. CAMSS then reports the width as
// bytesperline but pads each line to the stride implied by the plane size, for example
// 1152 bytes for 1056 pixels (1769472 = 1152 * 1024 * 3 / 2).
unsigned line_stride(const v4l2_pix_format_mplane &pixels) {
    const auto &plane = pixels.plane_fmt[0];
    if (pixels.pixelformat != V4L2_PIX_FMT_NV12 || !pixels.height ||
        plane.sizeimage * 2ull % (pixels.height * 3ull))
        return plane.bytesperline;
    return std::max<unsigned>(plane.bytesperline, plane.sizeimage * 2ull / (pixels.height * 3ull));
}

template <typename Item> uint64_t pointer(std::vector<Item> &items) {
    return reinterpret_cast<uintptr_t>(items.data());
}

// Frame sensor nodes carry their calibration camera name, such as "slam_left", as the
// device-tree "id" property.
std::string sensor_id(uint32_t major_number, uint32_t minor_number) {
    std::ifstream file("/sys/dev/char/" + std::to_string(major_number) + ":" +
                       std::to_string(minor_number) + "/device/of_node/id");
    std::string id;
    std::getline(file, id, '\0');
    return id;
}

// Returns the device-tree ids of the sensors that feed a video node through enabled media
// links. XRService does not route each sensor through the same CSID and VFE on every Frame, so
// the video node name alone does not identify a camera.
std::vector<std::string> video_sources(dev_t video) {
    std::vector<std::string> sources;
    for (int index = 0; index < 16 && sources.empty(); ++index) {
        FileDescriptor media(
            open(("/dev/media" + std::to_string(index)).c_str(), O_RDONLY | O_CLOEXEC));
        media_v2_topology topology{};
        if (!media || ioctl_next()(media.get(), MEDIA_IOC_G_TOPOLOGY, &topology) < 0)
            continue;
        std::vector<media_v2_entity> entities(topology.num_entities);
        std::vector<media_v2_interface> interfaces(topology.num_interfaces);
        std::vector<media_v2_pad> pads(topology.num_pads);
        std::vector<media_v2_link> links(topology.num_links);
        topology.ptr_entities = pointer(entities);
        topology.ptr_interfaces = pointer(interfaces);
        topology.ptr_pads = pointer(pads);
        topology.ptr_links = pointer(links);
        if (ioctl_next()(media.get(), MEDIA_IOC_G_TOPOLOGY, &topology) < 0 ||
            topology.num_entities != entities.size() ||
            topology.num_interfaces != interfaces.size() || topology.num_pads != pads.size() ||
            topology.num_links != links.size())
            continue;
        const auto interface_link = [&](uint32_t id, bool by_interface) {
            return std::find_if(links.begin(), links.end(), [&](const auto &link) {
                return (link.flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_INTERFACE_LINK &&
                       (by_interface ? link.source_id : link.sink_id) == id;
            });
        };
        const auto pad_entity = [&](uint32_t id) {
            const auto pad = std::find_if(pads.begin(), pads.end(),
                                          [id](const auto &item) { return item.id == id; });
            return pad == pads.end() ? 0 : pad->entity_id;
        };
        const auto node = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto &item) {
            return item.intf_type == MEDIA_INTF_T_V4L_VIDEO && item.devnode.major == major(video) &&
                   item.devnode.minor == minor(video);
        });
        if (node == interfaces.end())
            continue;
        const auto start = interface_link(node->id, true);
        if (start == links.end())
            continue;
        std::vector<uint32_t> walk{start->sink_id};
        for (size_t next = 0; next < walk.size() && walk.size() <= entities.size(); ++next) {
            for (const auto &link : links) {
                if ((link.flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK ||
                    !(link.flags & MEDIA_LNK_FL_ENABLED) || pad_entity(link.sink_id) != walk[next])
                    continue;
                const uint32_t source = pad_entity(link.source_id);
                if (!source || std::find(walk.begin(), walk.end(), source) != walk.end())
                    continue;
                walk.push_back(source);
                const auto entity =
                    std::find_if(entities.begin(), entities.end(),
                                 [source](const auto &item) { return item.id == source; });
                if (entity == entities.end() || entity->function != MEDIA_ENT_F_CAM_SENSOR)
                    continue;
                const auto subdev = interface_link(source, false);
                const auto sensor =
                    subdev == links.end()
                        ? interfaces.end()
                        : std::find_if(interfaces.begin(), interfaces.end(), [&](const auto &item) {
                              return item.id == subdev->source_id;
                          });
                if (sensor != interfaces.end() && sensor->intf_type == MEDIA_INTF_T_V4L_SUBDEV)
                    sources.push_back(sensor_id(sensor->devnode.major, sensor->devnode.minor));
            }
        }
    }
    return sources;
}

double clock_seconds(clockid_t clock) {
    timespec now{};
    clock_gettime(clock, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

bool sync_read(int dma, uint64_t phase) {
    dma_buf_sync request{DMA_BUF_SYNC_READ | phase};
    int result;
    do {
        result = ioctl_next()(dma, DMA_BUF_IOCTL_SYNC, &request);
    } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result == 0;
}

double sof_time(const v4l2_plane &metadata) {
    constexpr size_t Size = sizeof(uint64_t);
    if (metadata.data_offset || metadata.bytesused < Size ||
        !sync_read(metadata.m.fd, DMA_BUF_SYNC_START))
        return 0;
    void *bytes = mmap(nullptr, Size, PROT_READ, MAP_SHARED, metadata.m.fd, 0);
    uint64_t timestamp = 0;
    if (bytes != MAP_FAILED) {
        memcpy(&timestamp, bytes, Size);
        munmap(bytes, Size);
    }
    const bool synced = sync_read(metadata.m.fd, DMA_BUF_SYNC_END);
    // Qualcomm CAMSS frame header stores the ISP SOF counter in bits 63:8.
    return synced && !(timestamp & 0xff) ? (timestamp >> 8) / TimerHz : 0;
}

double capture_time(const v4l2_buffer &buffer, double sof) {
    // The Frame's CAMSS VFE 880 dequeues with ktime_get_raw_ns().
    const double end = buffer.timestamp.tv_sec + buffer.timestamp.tv_usec * 1e-6;
    const double raw = clock_seconds(CLOCK_MONOTONIC_RAW);
    if (end < raw - .100 || end > raw + .002)
        return 0;
#if defined(__aarch64__)
    uint64_t counter;
    asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
    const double start = raw + (sof - counter / TimerHz);
    if (start <= end + .002 && end - start <= .025)
        return start;
#else
    (void)sof;
#endif
    return end;
}

struct Group {
    std::array<View, ViewCount> views;
    double sof = 0;
    uint64_t seq = 0;
};

class CameraHook {
    using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
    std::mutex mutex;
    std::array<Camera, ViewCount> cameras{{
        {"slam_right", "msm_vfe3_video0", 1056, 1024, 1769472, {}},
        {"slam_left", "msm_vfe4_video0", 1056, 1024, 1769472, {}},
        {"upper_left", "msm_vfe2_video0", 640, 480, 462848, {}},
        {"upper_right", "msm_vfe2_video1", 640, 480, 462848, {}},
    }};
    std::unordered_map<int, Backing> backing;
    FileDescriptor lock, listener, reader, pidfd;
    std::string calibration, path;
    std::array<Group, 2> pending;
    Group leased;
    uint64_t sequence = 0;
    std::array<double, 2> last_published{};

    void clear(Group &group) {
        for (auto &view : group.views) {
            if (view.dma)
                sync_read(view.dma.get(), DMA_BUF_SYNC_END);
            if (view.queue.length) {
                view.queue.m.planes = view.planes.data();
                if (ioctl_next()(view.device, VIDIOC_QBUF, &view.queue) < 0)
                    dprintf(2, "Controller Utils camera: delayed QBUF failed: %s\n",
                            strerror(errno));
            }
        }
        group = {};
    }

    void service_reader() {
        if (reader) {
            char data[64];
            const ssize_t size = recv(reader.get(), data, sizeof(data), MSG_DONTWAIT | MSG_TRUNC);
            char expected[64];
            const int count = snprintf(expected, sizeof(expected), "{\"release\": %llu}",
                                       static_cast<unsigned long long>(leased.seq));
            if (leased.seq && size == count && !memcmp(data, expected, count))
                clear(leased);
            else if (!size || size > 0 || (errno != EAGAIN && errno != EINTR))
                reader.reset();
        }
        if (leased.seq) {
            pollfd state{pidfd.get(), POLLIN, 0};
            if (poll(&state, 1, 0) > 0 && state.revents & POLLIN) {
                reader.reset();
                clear(leased);
            }
        }
        if (!reader) {
            for (auto &group : pending)
                clear(group);
            if (leased.seq)
                return;
            pidfd.reset();
            reader.reset(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!reader)
                return;
            ucred peer{};
            socklen_t size = sizeof(peer);
            int process = -1;
            if (getsockopt(reader.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
                peer.uid != geteuid()) {
                reader.reset();
                return;
            }
            size = sizeof(process);
            if (getsockopt(reader.get(), SOL_SOCKET, SO_PEERPIDFD, &process, &size) < 0 ||
                send(reader.get(), calibration.data(), calibration.size(), MSG_NOSIGNAL) !=
                    static_cast<ssize_t>(calibration.size())) {
                FileDescriptor failed(process);
                reader.reset();
                return;
            }
            pidfd.reset(process);
            last_published = {};
        }
    }

    void publish(bool ir) {
        if (leased.seq)
            return;
        auto &group = pending[ir];
        std::array<int, ViewCount> descriptors{};
        const size_t count = ir ? ViewCount : FrontViews;
        double time = 0;
        std::string views;
        for (size_t i = 0; i < count; ++i) {
            const auto &view = group.views[i];
            if (!view.file)
                return;
            if (i < FrontViews)
                time += view.time / FrontViews;
            descriptors[i] = view.file.get();
            views += (i ? ",[\"" : "[\"") + cameras[i].serial + "\"," +
                     std::to_string(cameras[i].stride) + "," + std::to_string(view.offset) + "]";
        }
        const uint64_t seq = sequence + 1;
        std::string header = "{\"seq\":" + std::to_string(seq) +
                             ",\"time\":" + std::to_string(time) + ",\"exposure\":\"" +
                             (ir ? "ir" : "scene") + "\",\"storage\":\"udmabuf\",\"views\":[" +
                             views + "]}";
        alignas(cmsghdr) char control[CMSG_SPACE(ViewCount * sizeof(int))]{};
        iovec body{header.data(), header.size()};
        msghdr packet{};
        packet.msg_iov = &body;
        packet.msg_iovlen = 1;
        packet.msg_control = control;
        packet.msg_controllen = CMSG_SPACE(count * sizeof(int));
        auto *rights = CMSG_FIRSTHDR(&packet);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(rights), descriptors.data(), count * sizeof(int));
        if (sendmsg(reader.get(), &packet, MSG_NOSIGNAL) == static_cast<ssize_t>(header.size())) {
            sequence = group.seq = seq;
            last_published[ir] = group.sof;
            leased = std::move(group);
            group = {};
        } else {
            clear(group);
        }
    }

    void dequeue(size_t index, const v4l2_buffer &buffer) {
        auto &camera = cameras[index];
        if (buffer.memory != V4L2_MEMORY_DMABUF || buffer.length != 2 ||
            (buffer.flags & (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_ERROR)) !=
                V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
            return;
        service_reader();
        if (!reader)
            return;
        const double sof = sof_time(buffer.m.planes[1]);
        if (!sof)
            return;
        const double gap = sof - std::exchange(camera.last_sof, sof);
        // shortcut: the sensor mode interleaves an 8.38 ms SOF gap with a longer one that follows
        // the light-source frequency: 21.62 ms at 50 Hz (30 ms cycle) and 24.95 ms at 60 Hz
        // (33.3 ms cycle). Read the exposure registers if Valve adds another interleaving cadence.
        const bool ir = gap > .007 && gap < .010;
        const bool front = index < FrontViews;
        if (!ir && (!front || gap < .020 || gap > .026))
            return;
        if (sof <= last_published[ir] + Tolerance)
            return;
        auto &group = pending[ir];
        if (group.sof && std::abs(sof - group.sof) > Tolerance) {
            if (sof < group.sof)
                return;
            clear(group);
        }
        if (group.views[index].file)
            return;
        const double time = capture_time(buffer, sof);
        if (!time)
            return;
        const auto &pixels = buffer.m.planes[0];
        const auto source = backing.find(pixels.m.fd);
        struct stat dma{};
        if (source == backing.end() || pixels.data_offset ||
            pixels.bytesused < camera.stride * camera.height || fstat(pixels.m.fd, &dma) < 0 ||
            dma.st_ino != source->second.inode)
            return;
        FileDescriptor shared(fcntl(source->second.file.get(), F_DUPFD_CLOEXEC, 3));
        FileDescriptor lease(fcntl(pixels.m.fd, F_DUPFD_CLOEXEC, 3));
        if (!shared || !lease || !sync_read(lease.get(), DMA_BUF_SYNC_START))
            return;
        group.sof = sof;
        auto &view = group.views[index];
        view.file = std::move(shared);
        view.dma = std::move(lease);
        view.inode = dma.st_ino;
        view.offset = source->second.offset;
        view.time = time - (ir ? 0 : .00836);
        view.device = camera.device;
        publish(ir);
    }

  public:
    explicit CameraHook(const char *config) {
        std::ifstream input(config);
        std::string packet;
        if (!std::getline(input, packet) || packet.empty() || packet.size() > 131072)
            return;
        Json parsed(json_tokener_parse(packet.c_str()), json_object_put);
        json_object *version, *metadata, *entries;
        if (!parsed || !json_object_object_get_ex(parsed.get(), "version", &version) ||
            json_object_get_int(version) != 1 ||
            !json_object_object_get_ex(parsed.get(), "metadata", &metadata) ||
            !json_object_object_get_ex(metadata, "cameras", &entries))
            return;
        for (auto &camera : cameras) {
            json_object *entry, *serial;
            if (!json_object_object_get_ex(entries, camera.name, &entry) ||
                !json_object_object_get_ex(entry, "serialNumber", &serial) ||
                !json_object_is_type(serial, json_type_string))
                return;
            camera.serial = json_object_get_string(serial);
            if (camera.serial.empty() || camera.serial.size() > 64 ||
                camera.serial.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
                    std::string::npos)
                return;
        }
        const auto uid = geteuid();
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        std::string directory = runtime ? runtime : "/run/user/" + std::to_string(uid);
        struct stat info{};
        if (stat(directory.c_str(), &info) < 0 || !S_ISDIR(info.st_mode) || info.st_uid != uid ||
            (info.st_mode & 077))
            return;
        path = directory + "/controller-utils-camera.sock";
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path))
            return;
        lock.reset(open((path + ".lock").c_str(), O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600));
        if (!lock || flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
            return;
        if (lstat(path.c_str(), &info) == 0) {
            if (!S_ISSOCK(info.st_mode) || info.st_uid != uid)
                return;
            FileDescriptor probe(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            address.sun_family = AF_UNIX;
            memcpy(address.sun_path, path.c_str(), path.size() + 1);
            if (!probe ||
                connect(probe.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                    0 ||
                errno != ECONNREFUSED)
                return;
            if (unlink(path.c_str()) < 0)
                return;
        }
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        listener.reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!listener ||
            bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            chmod(path.c_str(), 0600) < 0 || listen(listener.get(), 2) < 0) {
            listener.reset();
            return;
        }
        calibration = std::move(packet);
        dprintf(2, "Controller Utils camera: sharing enabled\n");
    }

    void created(int fd, const udmabuf_create &request) {
        if (!listener || std::none_of(cameras.begin(), cameras.end(), [&](const auto &camera) {
                return request.size == camera.dma_size;
            }))
            return;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%u", request.memfd);
        FileDescriptor source(open(path, O_RDONLY | O_CLOEXEC));
        struct stat file{}, dma{};
        if (!source || fstat(source.get(), &file) < 0 || fstat(fd, &dma) < 0 ||
            !S_ISREG(file.st_mode) || file.st_size < 0 ||
            request.offset > static_cast<uint64_t>(file.st_size) ||
            request.size > static_cast<uint64_t>(file.st_size) - request.offset)
            return;
        const int seals = fcntl(source.get(), F_GET_SEALS);
        if (seals < 0 || !(seals & F_SEAL_SHRINK))
            return;
        std::lock_guard guard(mutex);
        for (auto entry = backing.begin(); entry != backing.end();) {
            struct stat current{};
            if (fstat(entry->first, &current) < 0 || current.st_ino != entry->second.inode)
                entry = backing.erase(entry);
            else
                ++entry;
        }
        if (backing.size() < 256)
            backing.insert_or_assign(fd, Backing{std::move(source), dma.st_ino, request.offset});
    }

    int camera_ioctl(int fd, unsigned long request, void *arg) {
        auto next = ioctl_next();
        if (!listener || !arg)
            return next(fd, request, arg);
        if (request == VIDIOC_REQBUFS) {
            std::lock_guard guard(mutex);
            int result = next(fd, request, arg);
            const int error = errno;
            auto &buffers = *static_cast<v4l2_requestbuffers *>(arg);
            struct stat dev{};
            if (!result && buffers.type == Capture && !fstat(fd, &dev) && S_ISCHR(dev.st_mode) &&
                major(dev.st_rdev) == 81) {
                std::ifstream device_name("/sys/dev/char/81:" + std::to_string(minor(dev.st_rdev)) +
                                          "/name");
                std::string video_name;
                std::getline(device_name, video_name);
                // Older device trees have no sensor ids; fall back to the original VFE routing.
                const auto sources = video_sources(dev.st_rdev);
                v4l2_format format{};
                format.type = Capture;
                const auto &pixels = format.fmt.pix_mp;
                const bool streaming = buffers.count >= MinBuffers &&
                                       buffers.memory == V4L2_MEMORY_DMABUF &&
                                       !next(fd, VIDIOC_G_FMT, &format) && pixels.num_planes == 2 &&
                                       // NV12 places the same 8-bit luma image first in plane 0.
                                       (pixels.pixelformat == V4L2_PIX_FMT_GREY ||
                                        pixels.pixelformat == V4L2_PIX_FMT_NV12);
                for (auto &camera : cameras) {
                    if (sources.empty() ? video_name != camera.video_name
                                        : std::find(sources.begin(), sources.end(), camera.name) ==
                                              sources.end())
                        continue;
                    // A shared CSIPHY can lead to two sensors; the frame size picks the camera.
                    const unsigned stride = line_stride(pixels);
                    if (streaming && pixels.width == camera.width &&
                        pixels.height == camera.height && stride >= camera.width &&
                        size_t{stride} * camera.height <= camera.dma_size) {
                        camera.device = fd;
                        camera.stride = stride;
                    } else if (camera.device == fd || sources.empty())
                        camera.device = -1;
                }
            }
            errno = error;
            return result;
        }
        if (request == VIDIOC_DQBUF) {
            const int result = next(fd, request, arg);
            const int error = errno;
            const auto &buffer = *static_cast<v4l2_buffer *>(arg);
            if (!result && buffer.type == Capture && buffer.m.planes) {
                std::lock_guard guard(mutex);
                for (size_t i = 0; i < cameras.size(); ++i)
                    if (cameras[i].device == fd) {
                        dequeue(i, buffer);
                        break;
                    }
            }
            errno = error;
            return result;
        }

        std::lock_guard guard(mutex);
        const auto camera = std::find_if(cameras.begin(), cameras.end(),
                                         [fd](const auto &item) { return item.device == fd; });
        if (camera == cameras.end())
            return next(fd, request, arg);
        const auto index = static_cast<size_t>(camera - cameras.begin());
        if (request == VIDIOC_STREAMOFF) {
            const int result = next(fd, request, arg);
            const int error = errno;
            if (result == 0) {
                leased.views[index].queue = {};
                for (auto &group : pending) {
                    group.views[index].queue = {};
                    clear(group);
                }
                camera->last_sof = 0;
            }
            errno = error;
            return result;
        }
        auto &buffer = *static_cast<v4l2_buffer *>(arg);
        if (buffer.type == Capture && buffer.memory == V4L2_MEMORY_DMABUF && buffer.length == 2 &&
            buffer.m.planes) {
            service_reader();
            for (auto *group : {&leased, &pending[0], &pending[1]}) {
                auto &view = group->views[index];
                struct stat dma{};
                if (view.file && !fstat(buffer.m.planes[0].m.fd, &dma) &&
                    dma.st_ino == view.inode) {
                    // The remaining buffers continue streaming even if the reader is suspended.
                    view.queue = buffer;
                    std::copy_n(buffer.m.planes, 2, view.planes.begin());
                    return 0;
                }
            }
        }
        return next(fd, request, arg);
    }
};

CameraHook *camera_hook() {
    static auto *hook = []() -> CameraHook * {
        const char *config = getenv("CONTROLLER_UTILS_CAMERA_CALIBRATION");
        char executable[1024]{};
        if (!config || readlink("/proc/self/exe", executable, sizeof(executable) - 1) <= 0)
            return nullptr;
        const char *name = strrchr(executable, '/');
        return name && !strcmp(name, "/XRService") ? new CameraHook(config) : nullptr;
    }();
    return hook;
}
} // namespace

extern "C" __attribute__((visibility("default"))) int ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    va_start(arguments, request);
    const auto arg = va_arg(arguments, uintptr_t);
    va_end(arguments);
    const int previous = errno;
    if (request == VIDIOC_REQBUFS || request == VIDIOC_DQBUF || request == VIDIOC_QBUF ||
        request == VIDIOC_STREAMOFF) {
        if (auto *hook = camera_hook()) {
            errno = previous;
            return hook->camera_ioctl(fd, request, reinterpret_cast<void *>(arg));
        }
    }
    errno = previous;
    auto next = ioctl_next();
    const int result = next(fd, request, arg);
    const int error = errno;
    if (result >= 0 && request == UDMABUF_CREATE && arg)
        if (auto *hook = camera_hook())
            hook->created(result, *reinterpret_cast<const udmabuf_create *>(arg));
    errno = error;
    return result;
}
