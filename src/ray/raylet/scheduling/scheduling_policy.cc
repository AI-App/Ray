// Copyright 2021 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/raylet/scheduling/scheduling_policy.h"

#include <functional>

namespace ray {

namespace raylet_scheduling_policy {
namespace {

bool IsGPURequest(const ResourceRequest &resource_request) {
  if (resource_request.predefined_resources.size() <= GPU) {
    return false;
  }
  return resource_request.predefined_resources[GPU] > 0;
}

bool DoesNodeHaveGPUs(const NodeResources &resources) {
  if (resources.predefined_resources.size() <= GPU) {
    return false;
  }
  return resources.predefined_resources[GPU].total > 0;
}
}  // namespace

int64_t SchedulingPolicy::HybridPolicyWithFilter(
    const ResourceRequest &resource_request, float spread_threshold, bool force_spillback,
    bool require_available, std::function<bool(int64_t)> is_node_available,
    NodeFilter node_filter) {
  auto predicate = [node_filter, &is_node_available](
                       int64_t node_id, const NodeResources &node_resources) {
    if (!is_node_available(node_id)) {
      return false;
    }
    if (node_filter == NodeFilter::kAny) {
      return true;
    }
    const bool has_gpu = DoesNodeHaveGPUs(node_resources);
    if (node_filter == NodeFilter::kGPU) {
      return has_gpu;
    }
    RAY_CHECK(node_filter == NodeFilter::kNonGpu);
    return !has_gpu;
  };

  // Step 1: Generate the traversal order. We guarantee that the first node is local, to
  // encourage local scheduling. The rest of the traversal order should be globally
  // consistent, to encourage using "warm" workers.
  std::vector<int64_t> round;
  round.reserve(nodes_.size());
  round.emplace_back(local_node_id_);

  for (const auto &pair : nodes_) {
    if (pair.first != local_node_id_) {
      round.emplace_back(pair.first);
    }
  }
  // Sort all the nodes, making sure that the local node stays in front.
  std::sort(round.begin() + 1, round.end());

  int64_t best_node_id = -1;
  float best_utilization_score = INFINITY;
  bool best_is_available = false;

  // Step 2: Perform the round robin.
  size_t round_index = spread_threshold == 0 ? spread_scheduling_next_index_ : 0;
  for (size_t i = 0; i < round.size(); ++i, ++round_index) {
    const auto &node_id = round[round_index % round.size()];
    const auto &it = nodes_.find(node_id);
    RAY_CHECK(it != nodes_.end());
    const auto &node = it->second;
    if (node_id == local_node_id_ && force_spillback) {
      continue;
    }
    if (!predicate(node_id, node.GetLocalView()) ||
        !node.GetLocalView().IsFeasible(resource_request)) {
      continue;
    }

    bool ignore_pull_manager_at_capacity = false;
    if (node_id == local_node_id_) {
      // It's okay if the local node's pull manager is at
      // capacity because we will eventually spill the task
      // back from the waiting queue if its args cannot be
      // pulled.
      ignore_pull_manager_at_capacity = true;
    }
    bool is_available = node.GetLocalView().IsAvailable(resource_request,
                                                        ignore_pull_manager_at_capacity);
    float critical_resource_utilization =
        node.GetLocalView().CalculateCriticalResourceUtilization();
    RAY_LOG(DEBUG) << "Node " << node_id << " is "
                   << (is_available ? "available" : "not available") << " for request "
                   << resource_request.DebugString()
                   << " with critical resource utilization "
                   << critical_resource_utilization << " based on local view "
                   << node.GetLocalView().DebugString(StringIdMap());
    if (critical_resource_utilization < spread_threshold) {
      critical_resource_utilization = 0;
    }

    bool update_best_node = false;

    if (is_available) {
      // Always prioritize available nodes over nodes where the task must be queued first.
      if (!best_is_available) {
        update_best_node = true;
      } else if (critical_resource_utilization < best_utilization_score) {
        // Break ties between available nodes by their critical resource utilization.
        update_best_node = true;
      }
    } else if (!best_is_available &&
               critical_resource_utilization < best_utilization_score &&
               !require_available) {
      // Pick the best feasible node by critical resource utilization.
      update_best_node = true;
    }

    if (update_best_node) {
      if (spread_threshold == 0) {
        spread_scheduling_next_index_ = ((round_index + 1) % round.size());
      }
      best_node_id = node_id;
      best_utilization_score = critical_resource_utilization;
      best_is_available = is_available;
    }
  }

  return best_node_id;
}

int64_t SchedulingPolicy::HybridPolicy(const ResourceRequest &resource_request,
                                       float spread_threshold, bool force_spillback,
                                       bool require_available,
                                       std::function<bool(int64_t)> is_node_available,
                                       bool scheduler_avoid_gpu_nodes) {
  if (!scheduler_avoid_gpu_nodes || IsGPURequest(resource_request)) {
    return HybridPolicyWithFilter(resource_request, spread_threshold, force_spillback,
                                  require_available, std::move(is_node_available));
  }

  // Try schedule on non-GPU nodes.
  auto best_node_id = HybridPolicyWithFilter(
      resource_request, spread_threshold, force_spillback,
      /*require_available*/ true, is_node_available, NodeFilter::kNonGpu);
  if (best_node_id != -1) {
    return best_node_id;
  }

  // If we cannot find any available node from non-gpu nodes, fallback to the original
  // scheduling
  return HybridPolicyWithFilter(resource_request, spread_threshold, force_spillback,
                                require_available, is_node_available);
}

}  // namespace raylet_scheduling_policy
}  // namespace ray
