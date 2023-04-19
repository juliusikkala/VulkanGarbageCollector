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
#include "vkgc.hh"

namespace vkgc
{

garbage_collector::garbage_collector(VkDevice dev)
: dev(dev)
{
}

void garbage_collector::release(void* resource, std::function<void()>&& cleanup)
{
    std::unique_lock<std::mutex> lk(mutex);
    resources[resource].cleanup = std::move(cleanup);
    check_delete(resource);
}

void garbage_collector::release(VkSemaphore sem)
{
    std::unique_lock<std::mutex> lk(mutex);
    semaphore_dependencies[sem].should_destroy = true;
}

void garbage_collector::depend(void* used_resource, void* user_resource)
{
    depend_many(&used_resource, 1, user_resource);
}

void garbage_collector::depend_many(void** used_resources, size_t used_resource_count, void* user_resource)
{
    std::unique_lock<std::mutex> lk(mutex);
    auto& dependents = resources[user_resource].dependents;
    dependents.insert(dependents.end(), used_resources, used_resources + used_resource_count);
    for(size_t i = 0; i < used_resource_count; ++i)
        resources[used_resources[i]].dependency_count++;
}

void garbage_collector::depend(void* used_resource, VkSemaphore timeline, uint64_t value)
{
    std::unique_lock<std::mutex> lk(mutex);
    resources[used_resource].dependency_count++;
    semaphore_info& sem = semaphore_dependencies[timeline];
    sem.triggers.push({value, used_resource});
}

void garbage_collector::collect()
{
    std::unique_lock<std::mutex> lk(mutex);
    for(auto it = semaphore_dependencies.begin(); it != semaphore_dependencies.end();)
    {
        uint64_t value = 0;
        vkGetSemaphoreCounterValue(dev, it->first, &value);

        auto& triggers = it->second.triggers;
        while(!triggers.empty() && triggers.top().value <= value)
        {
            if(triggers.top().callback)
                triggers.top().callback();

            if(triggers.top().dependent)
            {
                resources[triggers.top().dependent].dependency_count--;
                check_delete(triggers.top().dependent);
            }
            triggers.pop();
        }

        if(triggers.empty() && it->second.should_destroy)
        {
            vkDestroySemaphore(dev, it->first, nullptr);
            it = semaphore_dependencies.erase(it);
        }
        else ++it;
    }
}

void garbage_collector::wait_collect()
{
    collect();
    std::unique_lock<std::mutex> lk(mutex);
    if(resources.size() != 0 || semaphore_dependencies.size() != 0)
    {
        vkDeviceWaitIdle(dev);
        lk.unlock();
        collect();
    }
}

void garbage_collector::add_trigger(
    VkSemaphore timeline,
    uint64_t value,
    std::function<void()>&& callback
){
    std::unique_lock<std::mutex> lk(mutex);
    semaphore_info& sem = semaphore_dependencies[timeline];
    sem.triggers.push({value, nullptr, std::move(callback)});
}

bool garbage_collector::trigger::operator<(const trigger& t) const
{
    return t.value < value;
}

void garbage_collector::check_delete(void* resource)
{
    auto it = resources.find(resource);
    if(it->second.dependency_count == 0 && it->second.cleanup)
    {
        it->second.cleanup();
        for(void* dep: it->second.dependents)
        {
            resources[dep].dependency_count--;
            check_delete(dep);
        }
        resources.erase(it);
    }
}

}
