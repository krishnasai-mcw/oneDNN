/*******************************************************************************
 * Copyright 2021-2025 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#ifndef GRAPH_BACKEND_DNNL_PASSES_MEMORY_PLANNING_HPP
#define GRAPH_BACKEND_DNNL_PASSES_MEMORY_PLANNING_HPP

#include <algorithm>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

#include "graph/interface/value.hpp"

#include "graph/utils/utils.hpp"

#include "graph/backend/dnnl/scratchpad.hpp"
#include "graph/backend/dnnl/subgraph.hpp"

#include "oneapi/dnnl/dnnl.hpp"

namespace dnnl {
namespace impl {
namespace graph {
namespace dnnl_impl {

// info needed to create memory for host scalars at execution time
struct host_scalar_info_t {
    // index of executable in the compiled_partition
    size_t exec_idx;
    // input idx of the compiled_partition
    size_t input_idx;
    // dnnl_arg of the input
    int arg;
    // memory desc
    memory::desc md;
};

// This execution_args_set_t class is used to hold the dnnl memory objects which
// are used when executing a compiled subgraph in a thread. This class should
// only be generated by the memory_planner_t class. When executing subgraph in
// multi-threads, each thread should have a replica.
class execution_args_set_t {
public:
    execution_args_set_t() = default;

    execution_args_set_t(const execution_args_set_t &) = delete;
    execution_args_set_t(execution_args_set_t &&) = delete;
    execution_args_set_t &operator=(const execution_args_set_t &) = delete;
    execution_args_set_t &operator=(execution_args_set_t &&) = delete;

    // Deep copy
    std::shared_ptr<execution_args_set_t> clone() const;

    void clear();

    // getters
    const std::vector<exec_args> &get_exec_args() const {
        return topo_ordered_exec_args_;
    }

    const std::unordered_map<value_t *, memory> &get_value_mem_map() const {
        return value_mem_map_;
    }

    const std::vector<host_scalar_info_t> &get_host_scalar_infos() const {
        return host_scalar_infos_;
    }

    const std::vector<std::pair<dnnl::memory, size_t>> &
    get_mems_use_external_inputs() const {
        return mems_use_external_inputs_;
    }

    const std::vector<std::pair<dnnl::memory, size_t>> &
    get_mems_use_external_outputs() const {
        return mems_use_external_outputs_;
    }

    const std::vector<std::pair<dnnl::memory, size_t>> &
    get_mems_use_internal_temporary() const {
        return mems_use_internal_temporary_;
    }

    const std::vector<std::pair<dnnl::memory, size_t>> &
    get_mems_use_internal_persistent() const {
        return mems_use_internal_persistent_;
    }

    std::vector<dnnl::memory::desc> get_persistent_mem_desc_list() const {
        std::vector<dnnl::memory::desc> mds;
        mds.reserve(mems_use_internal_persistent_.size());
        for (auto &mem_offkey : mems_use_internal_persistent_) {
            auto md = mem_offkey.first.get_desc();
            mds.emplace_back(md);
        }
        return mds;
    }

    // adders
    void add_exec_args(const exec_args &args) {
        topo_ordered_exec_args_.emplace_back(args);
    }

    void reset_exec_args(size_t idx, const exec_args &args) {
        if (idx >= topo_ordered_exec_args_.size()) return;
        topo_ordered_exec_args_[idx] = args;
    }

    void add_host_scalar_arg(
            size_t input_idx, const memory::desc &md, int arg) {
        host_scalar_info_t scalar_info;
        scalar_info.input_idx = input_idx;
        scalar_info.md = md;
        scalar_info.arg = arg;
        size_t exec_idx = topo_ordered_exec_args_.size();
        scalar_info.exec_idx = exec_idx;
        host_scalar_infos_.emplace_back(scalar_info);
    }

    void add_value_mem_map(const std::pair<value_t *, memory> &map) {
        value_mem_map_.insert(map);
    }

    void add_mem_use_external_inputs(
            const std::pair<dnnl::memory, size_t> &mem_idx) {
        mems_use_external_inputs_.emplace_back(mem_idx);
    }

    void add_mem_use_external_outputs(
            const std::pair<dnnl::memory, size_t> &mem_idx) {
        mems_use_external_outputs_.emplace_back(mem_idx);
    }

    void add_mem_use_internal_temporary(
            const std::pair<dnnl::memory, size_t> &mem_offkey) {
        mems_use_internal_temporary_.emplace_back(mem_offkey);
    }

    void add_mem_use_internal_persistent(
            const std::pair<dnnl::memory, size_t> &mem_offkey) {
        mems_use_internal_persistent_.emplace_back(mem_offkey);
    }

    // finders
    bool find_value_mem_map(value_t *key, memory &mem) const {
        auto pos = value_mem_map_.find(key);
        if (pos != value_mem_map_.end()) {
            mem = pos->second;
            return true;
        }
        return false;
    }

private:
    // memory <-> index of used underlying buffer in the given inputs list
    std::vector<std::pair<dnnl::memory, size_t>> mems_use_external_inputs_;
    // memory <-> index of used underlying buffer in the given outputs list
    std::vector<std::pair<dnnl::memory, size_t>> mems_use_external_outputs_;
    // memory <-> offset key of used underlying buffer in the internal temporary
    // registry
    std::vector<std::pair<dnnl::memory, size_t>> mems_use_internal_temporary_;
    // memory <-> offset key of used underlying buffer in the internal
    // persistent registry
    std::vector<std::pair<dnnl::memory, size_t>> mems_use_internal_persistent_;
    // value pointer -> memory
    std::unordered_map<value_t *, memory> value_mem_map_;
    // execution args for each op in the subgraph
    std::vector<exec_args> topo_ordered_exec_args_;
    // host scalar info
    std::vector<host_scalar_info_t> host_scalar_infos_;
};

class alias_analyzer_t {
public:
    alias_analyzer_t() = default;

    void clear();

    status_t run(std::shared_ptr<subgraph_t> &sg);

    // one input can alias to multiple output
    std::vector<const value_t *> get_alias_outputs(const value_t *input) const;

    // a output can alias to only one input
    const value_t *get_alias_input(const value_t *output) const;

    std::vector<const value_t *> get_all_aliases(const value_t *val) const;

private:
    // output->input
    std::unordered_map<const value_t *, const value_t *> alias_map_;
    // input->output
    // reverse_alias_map: map from in_value to out_value
    // it can be multimap, meaning one in_value can be used to
    // generate several out_values, which is used in the case
    // of input alias, i.e. conv+add: conv's first input is also
    // used for add's second input.
    std::unordered_multimap<const value_t *, const value_t *>
            reverse_alias_map_;
};

// The buffer_assigner_t class acts like a memory pool, but it doesn't hold real
// buffer but buffer_info_t (we call it as buffer in following description for
// convenience). The assigner maintains a list of allocated buffers and a list
// of freed buffers. When users request a buffer from the assigner, the assigner
// will first look up the free list to see if there is a freed buffer can meet
// the request. If found, return it directly. If not found, the assigner will
// allocate a new buffer and return it. When users free a buffer, the assigner
// will put it into the free list for next usage.
class buffer_assigner_t {
public:
    // constructor
    explicit buffer_assigner_t(const size_t match_range)
        : match_range_(match_range) {}

    // request a free buffer
    size_t request(size_t size) {
        if (size == 0) return -1;
        // search buffers in [size / match_range_, size * match_range_)
        if (match_range_ == 0) return this->alloc(size);
        auto begin = free_.lower_bound(size / match_range_);
        auto mid = free_.lower_bound(size);
        auto end = free_.upper_bound(size * match_range_);
        // search for buffers larger than requested
        auto it = mid;
        if (it != end) {
            buffer_info_t *e = it->second;
            // Use exact matching strategy
            e->max_bytes_ = std::max(size, e->max_bytes_);
            // find a exact match, erase from map and return
            free_.erase(it);
            return e->id_;
        }
        // then search for buffers smaller than requested space
        if (it != begin) {
            --it;
            buffer_info_t *e = it->second;
            // Use exact matching strategy
            e->max_bytes_ = std::max(size, e->max_bytes_);
            // erase from map and return
            free_.erase(it);
            return e->id_;
        }
        // cannot find anything return a new one.
        return this->alloc(size);
    }

    // release a buffer.
    void release(size_t id) {
        assertm(id < data_.size() || id == static_cast<size_t>(-1),
                "invalid buffer id");
        if (id == static_cast<size_t>(-1)) return;
        buffer_info_t *e = data_[id].get();
        free_.insert({e->max_bytes_, e});
    }

    // return the size of a buffer
    size_t query_size(size_t id) const {
        assertm(id < data_.size() || id == static_cast<size_t>(-1),
                "invalid buffer id");
        if (id == static_cast<size_t>(-1)) return 0;
        return data_[id]->max_bytes_;
    }

    void clear() {
        free_.clear();
        data_.clear();
    }

private:
    size_t alloc(size_t size) {
        size_t id = static_cast<size_t>(data_.size());
        std::unique_ptr<buffer_info_t> ptr(new buffer_info_t(id, size));
        data_.emplace_back(std::move(ptr));
        return id;
    }

    struct buffer_info_t {
        buffer_info_t(size_t id, size_t size) : id_(id), max_bytes_(size) {};
        // the id of the buffer.
        size_t id_;
        // maximum size of buffer requested.
        size_t max_bytes_;
    };

    // scale used for rough match
    size_t match_range_;
    // list of freed buffers
    std::multimap<size_t, buffer_info_t *> free_;
    // all the available buffers
    std::vector<std::unique_ptr<buffer_info_t>> data_;
};

// This memory_planner_t class is used to plan which buffer can be used by each
// value in the subgraph. All the planning works are completed in compilation
// stage for static shape cases.
//
// The available buffers will be one of the followings:
// - external inputs buffers given by users
// - external outputs buffers given by users
// - internal temporary buffers provided by a scratchpad (the scratchpad may be
//   allocated inside library or given by users)
// - internal persistent buffers which will be cached into the global constant
//   cache (the buffer is allocated inside the library at this moment)
//
// The supported memory sharing policy:
// - Inplace sharing. Use same buffer for input and output values of ops that
//   support inplace computation.
// - Standard sharing. Use same buffer for values that have disjoint live range.
//   Take this subgraph 't1 -> op1 -> t2 -> op2 -> t3 -> op3 -> t4-> op4 -> t5'
//   as an example: when writing data to t4, t2 is not used any more, so they
//   have disjoint live range and we can make them share same buffer.
//
// The following internal env vars can be used to control the memory planning:
// - _ONEDNN_GRAPH_ENABLE_MEM_REUSE
//     - 0: Disable memory sharing
//     - 1 (default): Enable memory sharing
class memory_planner_t {
public:
    memory_planner_t()
        : persistent_buffer_assigner_(16), temporary_buffer_assigner_(16) {}

    memory_planner_t(memory_planner_t &&) = delete;
    memory_planner_t(const memory_planner_t &other) = delete;
    memory_planner_t &operator=(const memory_planner_t &) = delete;
    memory_planner_t &operator=(memory_planner_t &&) = delete;

    grantor_t internal_persistent_grantor(char *base_ptr) const {
        return persistent_registry_.grantor(base_ptr);
    }

    grantor_t internal_temporary_grantor(char *base_ptr) const {
        return temporary_registry_.grantor(base_ptr);
    }

    size_t total_internal_persistent_size() const {
        return persistent_registry_.size();
    }

    size_t total_internal_temporary_size() const {
        return temporary_registry_.size();
    }

    execution_args_set_t &get_exec_args_set() { return exec_args_set_; }

    status_t run(std::shared_ptr<subgraph_t> &sg);

    const std::vector<inplace_pair_t> &get_subgraph_inplace_pairs() const {
        return inplace_pairs_;
    };

    std::string get_memory_info(const value_t *val) const {
        std::string str;
        auto pos = buffer_assignments_.find(val);
        if (pos == buffer_assignments_.end()) return str;

        assign_info_t info = pos->second;
        if (info.kind_ == internal_persistent) {
            str += "persistent_";
        } else if (info.kind_ == internal_temporary) {
            str += "temporary_";
        } else if (info.kind_ == external_input) {
            str += "external_in_";
        } else if (info.kind_ == external_output) {
            str += "external_out_";
        } else {
        }

        str += std::to_string(info.index_);
        return str;
    }

private:
    enum buffer_kind_t {
        external_input = 0,
        external_output,
        internal_temporary,
        internal_persistent,
    };

    class assign_info_t {
    public:
        assign_info_t(buffer_kind_t kind, size_t index)
            : kind_(kind), index_(index) {}

        assign_info_t() = default;
        assign_info_t(const assign_info_t &other) = default;
        assign_info_t &operator=(const assign_info_t &other) = default;

        bool operator==(const assign_info_t &other) const {
            return kind_ == other.kind_ && index_ == other.index_;
        }

        bool operator!=(const assign_info_t &other) const {
            return !(*this == other);
        }

        buffer_kind_t kind_;
        size_t index_; // the index to allocated buffer
    };

    struct time_bound_t {
        size_t start_;
        size_t end_;
    };

    void clear() {
        alias_analyzer_.clear();
        buffer_assignments_.clear();
        exec_args_set_.clear();
        persistent_buffer_assigner_.clear();
        temporary_buffer_assigner_.clear();
        persistent_registry_.clear();
        temporary_registry_.clear();
        external_inputs_live_range_.clear();
        inplace_pairs_.clear();
    }

    status_t assign_external_inputs_buffer(std::shared_ptr<subgraph_t> &sg,
            const std::vector<logical_tensor_t> &inputs);

    status_t assign_external_outputs_buffer(std::shared_ptr<subgraph_t> &sg,
            const std::vector<logical_tensor_t> &outputs,
            fusion_info_mgr_t &mgr);

    status_t assign_internal_persistent_buffer(
            std::shared_ptr<subgraph_t> &sg, fusion_info_mgr_t &mgr);

    status_t assign_internal_temporary_buffer(std::shared_ptr<subgraph_t> &sg,
            const std::unordered_map<value_t *, size_t> &edge_ref_count,
            fusion_info_mgr_t &mgr, bool enable_standard_sharing);

    status_t prepare_subgraph_inplace_pairs(
            std::shared_ptr<subgraph_t> &sg, bool enable_standard_sharing);

    status_t book_buffers(std::shared_ptr<subgraph_t> &sg);

    status_t prepare_execution_args_set(std::shared_ptr<subgraph_t> &sg,
            const dnnl::engine &p_engine, fusion_info_mgr_t &mgr);

    execution_args_set_t exec_args_set_;

    std::unordered_map<const value_t *, assign_info_t> buffer_assignments_;

    buffer_assigner_t persistent_buffer_assigner_;
    buffer_assigner_t temporary_buffer_assigner_;
    registry_t persistent_registry_;
    registry_t temporary_registry_;

    alias_analyzer_t alias_analyzer_;
    std::unordered_map<const assign_info_t *, time_bound_t>
            external_inputs_live_range_;
    std::vector<inplace_pair_t> inplace_pairs_;
};

} // namespace dnnl_impl
} // namespace graph
} // namespace impl
} // namespace dnnl

#endif
