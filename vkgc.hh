/*
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org>
*/
#ifndef VKGC_GARBAGE_COLLECTOR_HH
#define VKGC_GARBAGE_COLLECTOR_HH

#include <vulkan/vulkan.h>
//#include "volk.h"

#include <functional>
#include <queue>
#include <unordered_map>
#include <mutex>

namespace vkgc
{

// A thread-safe garbage collector for Vulkan resources. It's based on tracking
// resource inter-dependencies, which you have to report yourself by calling the
// depend() function (e.g. image views must depend on the image).
//
// At the top level are command buffers, which have no dependees, but they can
// only be removed when they are not running. To achieve this, you need to use
// the depend() overload which accepts a timeline semaphore and a value that is
// signalled once the command buffer has finished running.
class garbage_collector
{
public:
    garbage_collector(VkDevice dev);
    garbage_collector(const garbage_collector&) = delete;
    garbage_collector(garbage_collector&& other) noexcept = delete;
    ~garbage_collector() = default;

    // When you do not need to refer to a resource on the CPU side anymore,
    // you must call this function to let the GC know that it can be collected
    // once no GPU resources refer to it anymore. You should call this in a
    // RAII-style destructor, e.g. destructor of a buffer class.
    // You should never add new dependencies to resources you have already
    // released.
    void release(void* resource, std::function<void()>&& cleanup);

    // Semaphores are a special case and need to be released with this function.
    // vkDestroySemaphore will be called once nothing waits for the semaphore
    // anymore.
    void release(VkSemaphore sem);

    // Checks all known semaphores and recursively destroys released resources
    // that are no longer referenced by running command buffers or other
    // resources. This should be called periodically, e.g. while waiting for
    // vsync.
    void collect();

    // collect() but with a vkDeviceWaitIdle(). You can call this at the end of
    // your program, right before destroying the VkDevice, to make sure that
    // everything is properly released.
    void wait_collect();

    // The callback is called during collect() once the given timeline semaphore
    // hits the given value.
    void add_trigger(
        VkSemaphore timeline,
        uint64_t value,
        std::function<void()>&& callback
    );

    // Marks a dependency between two resources, where 'user_resource'
    // must be deleted before 'used_resource'.
    void depend(void* used_resource, void* user_resource);

    // Faster version of depend() for depending on many things simultaneously,
    // e.g. descriptor set depending on a pile of textures.
    void depend_many(void** used_resources, size_t used_resource_count, void* user_resource);

    // Makes sure that used_resource is not deleted before the given timeline
    // semaphore hits 'value'. Typically, used_resource would be a
    // VkCommandBuffer here.
    void depend(void* used_resource, VkSemaphore timeline, uint64_t value);

private:
    void check_delete(void* resource);

    std::mutex mutex;
    VkDevice dev;

    struct dependency_info
    {
        size_t dependency_count = 0;
        std::vector<void* /*resource*/> dependents;
        std::function<void()> cleanup;
    };

    std::unordered_map<void* /*resource*/, dependency_info> resources;

    struct trigger
    {
        uint64_t value;
        void* dependent;
        std::function<void()> callback;
        bool operator<(const trigger& t) const;
    };

    struct semaphore_info
    {
        std::priority_queue<trigger> triggers;
        bool should_destroy = false;
    };
    std::unordered_map<VkSemaphore, semaphore_info> semaphore_dependencies;
};

}

#endif
