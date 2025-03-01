// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <scheduler/reduction_utils.h>

#include <expr_evaluator.h>
#include <inlining.h>
#include <ir_cloner.h>
#include <ir_utils.h>
#include <maxinfo_propagator.h>
#include <ops/arith.h>
#include <scheduler/registry.h>
#include <scheduler/utils.h>
#include <transform_replay.h>

namespace nvfuser {

namespace reduction_scheduler_utils {

TensorView* scheduleReductionTV(
    const ReductionParams& rparams,
    TensorView* reduction_tv,
    bool has_iter_axis) {
  // Outer and inner reduction axis is relative. Outer reduce axis is only valid
  // in 3D scheduling. Otherwise inner_reduce_axis is the only reduction axis.
  // Inner here though is only relative to the other axis. When
  // rparams.fastest_dim == false, the reduction axis is logically outside the
  // iteration axis.
  const int iter_axis = 0;
  const int outer_reduce_axis = rparams.schedule_3D ? 1 : 0;
  const int inner_reduce_axis = rparams.schedule_3D ? 2 : has_iter_axis ? 1 : 0;

  const bool is_outer_grid_persistence = rparams.persistent_kernel &&
      rparams.cross_grid_inner_reduction && !rparams.fastest_dim;

  TORCH_INTERNAL_ASSERT(
      (int)reduction_tv->nDims() >
          std::max(iter_axis, std::max(outer_reduce_axis, inner_reduce_axis)),
      "Issue in scheduling reduction tv, expecting >",
      std::max(iter_axis, std::max(outer_reduce_axis, inner_reduce_axis)),
      " dimensions, but found ",
      reduction_tv->nDims());

  TORCH_INTERNAL_ASSERT(
      !(rparams.fastest_dim && rparams.vectorize_iter_dom),
      "Cannot vectorize iteration domain on inner reductions.");

  TORCH_INTERNAL_ASSERT(
      !(!rparams.fastest_dim && rparams.vectorize_inner_reduction),
      "Cannot vectorize reduction domain on outer reductions.");

  TORCH_INTERNAL_ASSERT(
      !(rparams.multiple_reds_per_blk && !has_iter_axis),
      "Multiple reductions requires an iter domain, but one wasn't found.");

  TORCH_INTERNAL_ASSERT(
      !(rparams.unroll_factor_iter_dom > 1 && !has_iter_axis),
      "Unrolling on iter domain requires an iter domain.");

  auto vectorize = [&reduction_tv](int axis, int factor) {
    reduction_tv->split(axis, factor);
    reduction_tv->axis(axis + 1)->parallelize(ParallelType::Vectorize);
  };

  auto inner_parallel = [&reduction_tv](int axis, ParallelType ptype) {
    reduction_tv->split(axis, NamedScalar::getParallelDim(ptype));
    reduction_tv->axis(axis + 1)->parallelize(ptype);
  };

  auto inner_parallel_static = [&reduction_tv](
                                   int axis, ParallelType ptype, int factor) {
    reduction_tv->split(axis, factor);
    reduction_tv->axis(axis + 1)->parallelize(ptype);
  };

  auto inner_unswitch = [&reduction_tv](int axis) {
    reduction_tv->split(axis, 1);
    reduction_tv->axis(axis + 1)->parallelize(ParallelType::Unswitch);
  };

  auto inner_unroll = [&reduction_tv](int axis, int factor) {
    reduction_tv->split(axis, factor);
    reduction_tv->axis(axis + 1)->parallelize(ParallelType::Unroll);
  };

  auto outer_parallel = [&reduction_tv](int axis, ParallelType ptype) {
    reduction_tv->split(axis, NamedScalar::getParallelDim(ptype), false);
    reduction_tv->axis(axis)->parallelize(ptype);
  };

  auto outer_unswitch = [&reduction_tv](int axis) {
    reduction_tv->split(axis, 1, false);
    reduction_tv->axis(axis)->parallelize(ParallelType::Unswitch);
  };

  auto outer_unroll = [&reduction_tv](int axis, int factor) {
    reduction_tv->split(axis, factor, false);
    reduction_tv->axis(axis)->parallelize(ParallelType::Unroll);
  };

  if (is_outer_grid_persistence) {
    const auto reduction_axis = inner_reduce_axis;
    TORCH_INTERNAL_ASSERT(rparams.static_bdimy, "blockDim.y must be static");
    inner_parallel_static(
        reduction_axis,
        rparams.block_dim_inner_reduction,
        rparams.lparams.bdimy());
    reduction_tv->split(
        reduction_axis, rparams.batches_per_block_inner_reduction);
    reduction_tv->axis(reduction_axis)
        ->parallelize(rparams.grid_dim_inner_reduction);
    // Unswitch the persistent buffer by a factor of
    // unroll_factor_inner_reduction. If that is equal to the
    // persistent buffer size, unswitch the whole buffer by
    // outer-unswith by 1. Otherwise, split the persistent buffer by
    // the unsiwtch factor and just unswitch the inner domain
    if (rparams.batches_per_block_inner_reduction ==
        rparams.unroll_factor_inner_reduction) {
      outer_unswitch(reduction_axis + 1);
    } else {
      reduction_tv->split(
          reduction_axis + 1, rparams.unroll_factor_inner_reduction);
      outer_unswitch(reduction_axis + 2);
    }
  } else if (rparams.persistent_kernel) {
    // Persistent Format:
    // [Grid Split, persistent buffer, unswitch, unroll, thread dim, vectorize]
    if (rparams.vectorize_inner_reduction) {
      vectorize(inner_reduce_axis, rparams.unroll_factor_inner_reduction);
    }
    auto outer_i = inner_reduce_axis;
    if (rparams.cross_grid_inner_reduction) {
      outer_parallel(outer_i++, rparams.grid_dim_inner_reduction);
    }

    reduction_tv->split(
        outer_i++, rparams.batches_per_block_inner_reduction, false);

    outer_unswitch(outer_i++);

    if (!rparams.vectorize_inner_reduction &&
        rparams.unroll_factor_inner_reduction > 1) {
      outer_unroll(outer_i++, rparams.unroll_factor_inner_reduction);
    }

    reduction_tv->axis(outer_i)->parallelize(rparams.block_dim_inner_reduction);

    if (rparams.pad_inner_reduction_to_warp) {
      reduction_tv->axis(outer_i)->padToMultipleOfWarp();
    }
  } else {
    // Non-persistent format:
    // [Grid Split, Remainder, unswitch, unroll, thread dim, vectorize]
    if (rparams.vectorize_inner_reduction) {
      vectorize(inner_reduce_axis, rparams.unroll_factor_inner_reduction);
    }

    if (rparams.cross_block_inner_reduction) {
      inner_parallel(inner_reduce_axis, rparams.block_dim_inner_reduction);
      if (rparams.pad_inner_reduction_to_warp) {
        reduction_tv->axis(inner_reduce_axis + 1)->padToMultipleOfWarp();
      }
    }

    if (!rparams.vectorize_inner_reduction &&
        rparams.unroll_factor_inner_reduction > 1) {
      inner_unroll(inner_reduce_axis, rparams.unroll_factor_inner_reduction);
    }

    inner_unswitch(inner_reduce_axis);
    if (rparams.cross_grid_inner_reduction) {
      if (rparams.split_grid_dim_inner_reduction) {
        outer_parallel(inner_reduce_axis, rparams.grid_dim_inner_reduction);
      } else {
        reduction_tv->axis(inner_reduce_axis)
            ->parallelize(rparams.grid_dim_inner_reduction);
      }
    }
  }

  // Outer reduction axis
  if (rparams.schedule_3D) {
    if (rparams.persistent_kernel) {
      // Persistent Format:
      // [Grid Split, persistent buffer, unroll, thread dim]
      auto outer_i = outer_reduce_axis;
      if (rparams.cross_grid_outer_reduction) {
        outer_parallel(outer_i++, rparams.grid_dim_outer_reduction);
      }

      reduction_tv->split(
          outer_i++, rparams.batches_per_block_outer_reduction, false);

      if (rparams.unroll_factor_outer_reduction > 1) {
        outer_unroll(outer_i++, rparams.unroll_factor_outer_reduction);
      }

      reduction_tv->axis(outer_i)->parallelize(
          rparams.block_dim_outer_reduction);
    } else {
      // Non-persistent format:
      // [Grid Split, Remainder, unroll, thread dim]
      if (rparams.cross_block_outer_reduction) {
        inner_parallel(outer_reduce_axis, rparams.block_dim_outer_reduction);
      }

      if (rparams.unroll_factor_outer_reduction > 1) {
        inner_unroll(outer_reduce_axis, rparams.unroll_factor_outer_reduction);
      }

      if (rparams.cross_grid_outer_reduction) {
        outer_parallel(outer_reduce_axis, rparams.grid_dim_outer_reduction);
      }
    }
  }

  // Iteration domain
  if (has_iter_axis) {
    // [Grid Split, unswitch, unroll, thread dim, vectorize]

    if (rparams.vectorize_iter_dom) {
      vectorize(iter_axis, rparams.unroll_factor_iter_dom);
    }

    if (isParallelTypeThread(rparams.block_dim_iter_dom)) {
      if (is_outer_grid_persistence) {
        TORCH_INTERNAL_ASSERT(
            rparams.static_bdimx, "blockDim.x must be static");
        inner_parallel_static(
            iter_axis, rparams.block_dim_iter_dom, rparams.lparams.bdimx());
      } else {
        inner_parallel(iter_axis, rparams.block_dim_iter_dom);
      }
    }

    if (!rparams.vectorize_iter_dom && rparams.unroll_factor_iter_dom > 1) {
      inner_unroll(iter_axis, rparams.unroll_factor_iter_dom);
    }

    // Do not unswitch interation domain in the case of outer grid
    // persistence as it's unclear if it's beneficial.
    if (rparams.unroll_factor_iter_dom > 1 && !is_outer_grid_persistence) {
      inner_unswitch(iter_axis);
    }

    if (isParallelTypeThread(rparams.grid_dim_iter_dom)) {
      if (rparams.split_grid_dim_iter_dom_outer) {
        outer_parallel(iter_axis, rparams.grid_dim_iter_dom);
      } else if (rparams.split_grid_dim_iter_dom_inner) {
        inner_parallel(iter_axis, rparams.grid_dim_iter_dom);
      } else {
        reduction_tv->axis(iter_axis)->parallelize(rparams.grid_dim_iter_dom);
      }
    }
  }

  auto reduction_rf_tv = sortAndRFactor(reduction_tv);

  // In the case of outer grid persistence, make sure the vectorized
  // domain placed at the innermost position.
  // TODO: Why isn't this the case by default?
  if (is_outer_grid_persistence) {
    int vec_id_cur_pos = -1;
    std::unordered_map<int, int> vec_reorder_map;
    for (const auto i : c10::irange(reduction_rf_tv->nDims())) {
      auto id = reduction_rf_tv->axis(i);
      if (id->getParallelType() == ParallelType::Vectorize) {
        vec_id_cur_pos = i;
        vec_reorder_map[i] = -1;
      } else if (vec_id_cur_pos >= 0) {
        vec_reorder_map[i] = i - 1;
      }
    }
    TORCH_INTERNAL_ASSERT(vec_id_cur_pos != -1, "Vectorized ID not found");
    reduction_rf_tv->reorder(vec_reorder_map);
  }

  return reduction_rf_tv;
}

namespace {

// Input: a vector of axes in the given tensor ignoring broadcasts. For example,
//        if you have a tensor T1[b, rS1, rS2, rS3], and you want to specify
//        axis rS2 and rS3, then your `non_broadcast_axes` should be {1, 2}.
// Output: the raw positions (counting broadcasts). In the above example, the
//         output should be {2, 3}.
std::vector<int> addBackBroadcasts(
    TensorView* tv,
    const std::unordered_set<int>& non_broadcast_axes) {
  // convert non-broadcast positions to raw positions
  std::vector<int> axes;
  int non_broadcast_pos = 0;
  for (const auto i : c10::irange(tv->nDims())) {
    if (tv->axis((int)i)->isBroadcast()) {
      continue;
    }
    if (non_broadcast_axes.count(non_broadcast_pos)) {
      axes.emplace_back(i);
    }
    non_broadcast_pos++;
  }
  return axes;
}

// Check if a reduction is effectively an allreduce.
bool isGridAllreduce(TensorView* reduction_tv) {
  // Only Local tensor is converted to allreduce
  if (reduction_tv->getMemoryType() != MemoryType::Local) {
    return false;
  }

  // Collect all reduction parallel types
  ParallelTypeBitmap reduction_parallel_types;
  std::for_each(
      reduction_tv->domain()->domain().begin(),
      reduction_tv->domain()->domain().end(),
      [&](auto id) {
        if (id->isReduction() &&
            isParallelTypeBlockDim(id->getParallelType())) {
          reduction_parallel_types.set(id->getParallelType());
        }
      });

  // If any of the reduction parallel types is used to parallelize
  // the broadcast, it will be converted to an allreduce reduction expr
  for (auto bcast_expr :
       ir_utils::filterByType<BroadcastOp>(reduction_tv->uses())) {
    auto bcast_tv = bcast_expr->out()->as<TensorView>();
    if (std::any_of(
            bcast_tv->domain()->domain().begin(),
            bcast_tv->domain()->domain().end(),
            [&](auto bcast_id) {
              auto pt = bcast_id->getParallelType();
              return isParallelTypeBlockDim(pt) &&
                  reduction_parallel_types.get(pt);
            })) {
      return true;
    }
  }
  return false;
}

} // namespace

void multiReductionInliner(
    Fusion* fusion,
    const ReductionParams& rparams,
    TensorView* reduction_tv,
    TensorView* reference_tv,
    std::vector<TensorView*> reduction_tvs,
    std::vector<TensorView*> cached_inputs,
    std::vector<std::pair<TensorView*, TensorView*>> cached_outputs,
    std::vector<TensorView*> dummy_outputs) {
  const bool is_outer_grid_persistence = rparams.persistent_kernel &&
      rparams.cross_grid_inner_reduction && !rparams.fastest_dim;

  // Propagate transformations before we rfactor the other reductions
  TransformPropagator propagator(reference_tv);
  MaxRootDomainInfoSpanningTree(reference_tv).traverse(&propagator);

  // If reduction_tv is rfactored, rfactor all reductions.
  if (reference_tv != reduction_tv) {
    // Apply rfactor to all reductions if applicable
    // We use axes ignoring broadcasts because in checkPatternEquivalence,
    // broadcast is ignored, we might end up having multiple reductions with
    // pattern equivalence but have different number of broadcasts, so the
    // position in the reference tensor is not necessary the same as the
    // position in other reduction TVs.
    std::unordered_set<int> non_broadcast_rfactor_axes;
    int non_broadcast_pos = 0;
    for (const auto i : c10::irange(reference_tv->nDims())) {
      if (reference_tv->axis((int)i)->isBroadcast()) {
        continue;
      }
      if (reference_tv->axis((int)i)->isReduction() &&
          reference_tv->axis((int)i)->isRFactorProduct()) {
        non_broadcast_rfactor_axes.insert(non_broadcast_pos);
      }
      non_broadcast_pos++;
    }

    for (auto reduction_tv_ : reduction_tvs) {
      if (reduction_tv_ == reduction_tv ||
          reduction_tv_->definition()->isA<GroupedReductionOp>()) {
        // This should come in already rfactored
        continue;
      } else {
        ir_utils::rfactorHelper(
            reduction_tv_,
            addBackBroadcasts(reduction_tv_, non_broadcast_rfactor_axes));
      }
    }
  }

  bool unroll = rparams.isUnrolled();

  bool vectorize =
      rparams.vectorize_inner_reduction || rparams.vectorize_iter_dom;

  // Propagate parallelization except vectorization and unrolling
  scheduler_utils::parallelizeAllLike(
      reference_tv,
      {},
      allParallelTypesExcept(
          {ParallelType::Unroll,
           ParallelType::Vectorize,
           ParallelType::MisalignedVectorize}));

  if (unroll) {
    // Find all tensor views that should have unroll or vectorization
    std::unordered_set<TensorView*> are_unrolled;

    // Grab all tensor views that should be vectorized
    auto vectorizable_inputs_outputs =
        scheduler_utils::getInputsOutputsWithInnerDim(reference_tv, true, true);

    auto vectorizable_expr = [](Expr* e) {
      return e->isA<UnaryOp>() &&
          e->as<UnaryOp>()->getUnaryOpType() == UnaryOpType::Set;
    };

    for (auto cached_input : cached_inputs) {
      if (vectorize) {
        auto producer_tvs = ir_utils::producerTvsOf(cached_input);
        if (producer_tvs.size() == 1 &&
            vectorizable_expr(cached_input->definition()) &&
            std::find(
                vectorizable_inputs_outputs.begin(),
                vectorizable_inputs_outputs.end(),
                producer_tvs[0]) != vectorizable_inputs_outputs.end()) {
          are_unrolled.emplace(cached_input);
        }
      } else {
        are_unrolled.emplace(cached_input);
      }
    }

    for (auto cached_output_pair : cached_outputs) {
      auto output = cached_output_pair.second;
      if (vectorize) {
        if (vectorizable_expr(output->definition()) &&
            std::find(
                vectorizable_inputs_outputs.begin(),
                vectorizable_inputs_outputs.end(),
                output) != vectorizable_inputs_outputs.end()) {
          are_unrolled.emplace(output);
        }
      } else {
        are_unrolled.emplace(output);
      }
    }

    if (!are_unrolled.empty()) {
      // Propagate vectorization/unrolling to those tensors that need it
      scheduler_utils::parallelizeAllLike(
          reference_tv,
          -1,
          {are_unrolled.begin(), are_unrolled.end()},
          {ParallelType::Unroll,
           ParallelType::Vectorize,
           ParallelType::MisalignedVectorize});
    }

    std::vector<TensorView*> rfactor_and_reduction_tvs = {
        reference_tv, reduction_tv};
    // If reference shouldn't be unrolled, clear that parallel type.
    // In the case of outer grid persistence, replace Vector with Group
    for (auto tv : rfactor_and_reduction_tvs) {
      if (are_unrolled.count(tv) == 0) {
        for (const auto i : c10::irange(tv->nDims())) {
          auto id = tv->axis((int)i);
          // Use Group only for grid reductions (i.e., not for rfactor'ed
          // reductions)
          if (is_outer_grid_persistence &&
              std::find(reduction_tvs.begin(), reduction_tvs.end(), tv) !=
                  reduction_tvs.end() &&
              id->getParallelType() == ParallelType::Vectorize) {
            tv->axis((int)i)->parallelize(ParallelType::Group);
            for (auto sibling : ir_utils::siblingTvsOf(tv)) {
              sibling->axis((int)i)->parallelize(ParallelType::Group);
            }
          } else if (
              id->getParallelType() == ParallelType::Unroll ||
              id->getParallelType() == ParallelType::Vectorize ||
              id->getParallelType() == ParallelType::MisalignedVectorize) {
            tv->axis((int)i)->parallelize(ParallelType::Serial);
            for (auto sibling : ir_utils::siblingTvsOf(tv)) {
              sibling->axis((int)i)->parallelize(ParallelType::Serial);
            }
          }
        }
      }
    }

    std::vector<TensorView*> allreduce_tvs;
    std::copy_if(
        reduction_tvs.begin(),
        reduction_tvs.end(),
        std::back_inserter(allreduce_tvs),
        [&](auto tv) { return reduction_tv != tv && isGridAllreduce(tv); });
    if (!allreduce_tvs.empty()) {
      scheduler_utils::parallelizeAllLike(
          reduction_tv, -1, allreduce_tvs, {ParallelType::Group});
    }
  }

  // Remove dummy outputs as they can inadvertently affect CA positions
  for (auto output : dummy_outputs) {
    fusion->removeOutput(output);
  }

  // Inline the schedule
  inlineMost();
}

namespace {

// Convert properties of an ID to a numeric value
int idPos(const IterDomain* id) {
  int inner_most = std::numeric_limits<int>::max();
  int outer_most = std::numeric_limits<int>::min();

  // Reduction and unrolled
  if (id->isReduction() &&
      (id->getParallelType() == ParallelType::Unroll ||
       id->getParallelType() == ParallelType::Vectorize ||
       id->getParallelType() == ParallelType::MisalignedVectorize)) {
    return inner_most;
  }
  inner_most--;

  // Reduction and constant
  if (id->isReduction() && id->extent()->isConstScalar()) {
    return inner_most;
  }
  inner_most--;

  // Reduction and unswitched
  if (id->isReduction() && id->getParallelType() == ParallelType::Unswitch) {
    return inner_most;
  }
  inner_most--;

  // Reduction and thread
  if (id->isReduction() && id->isThread()) {
    return inner_most;
  }
  inner_most--;

  // Broadcast
  if (id->isBroadcast() || id->isImplicitBroadcast()) {
    return inner_most;
  }
  inner_most--;

  // Iter and unrolled
  if (!id->isReduction() &&
      (id->getParallelType() == ParallelType::Unroll ||
       id->getParallelType() == ParallelType::Vectorize ||
       id->getParallelType() == ParallelType::MisalignedVectorize)) {
    return inner_most;
  }
  inner_most--;

  // Iter and unswitched
  if (!id->isReduction() && id->getParallelType() == ParallelType::Unswitch) {
    return inner_most;
  }
  inner_most--;

  // Reduction and non-constant
  if (id->isReduction() && !id->extent()->isConstScalar()) {
    return inner_most;
  }
  inner_most--;

  // Iter and block (outer)
  if (!id->isReduction() && id->isBlockDim()) {
    return outer_most;
  }
  outer_most++;

  // Iter and thread (outer)
  if (!id->isReduction() && id->isThreadDim()) {
    return outer_most;
  }
  outer_most++;

  // Iter and constant
  if (!id->isReduction() && id->extent()->isConstScalar()) {
    return outer_most;
  }
  outer_most++;

  // Iter and non-constant
  if (!id->isReduction() && !id->extent()->isConstScalar()) {
    return outer_most;
  }
  outer_most++;

  return 0;
}

struct id_lt {
  // Return if id0 should be before id1
  inline bool operator()(const IterDomain* id0, const IterDomain* id1) {
    return idPos(id0) < idPos(id1);
  }
};
} // namespace

TensorView* sortAndRFactor(TensorView* reference_tv) {
  auto domain = reference_tv->domain()->domain();
  std::sort(domain.begin(), domain.end(), id_lt());
  std::unordered_map<int, int> reorder_map;
  std::unordered_map<IterDomain*, int> domain_pos;
  for (int axis_i = 0; axis_i < (int)domain.size(); axis_i++) {
    domain_pos[domain[axis_i]] = axis_i;
  }
  for (int old_i = 0; old_i < (int)reference_tv->nDims(); old_i++) {
    auto new_i_it = domain_pos.find(reference_tv->axis(old_i));
    TORCH_INTERNAL_ASSERT(
        new_i_it != domain_pos.end(),
        "Error in schedule reorder, didn't reorder all axes in provided tv.");
    auto new_i = new_i_it->second;
    reorder_map[old_i] = new_i;
  }
  reference_tv->reorder(reorder_map);

  std::vector<int> rfactor_axes;
  std::vector<int> rfactor_axes_no_unswitch;
  size_t reduction_dims = 0;
  for (int axis_i = 0; axis_i < (int)reference_tv->nDims(); axis_i++) {
    auto id = reference_tv->axis(axis_i);
    if (!id->isReduction()) {
      continue;
    }

    reduction_dims++;
    if (id->isThread()) {
      continue;
    }

    // We always want an rfactor axis because our inlining logic expects it. If
    // there's no parallelization to split out, just rfactor everything but the
    // unswitch dim.
    if (!(id->getParallelType() == ParallelType::Unswitch &&
          id->extent()->isOneInt())) {
      rfactor_axes_no_unswitch.emplace_back(axis_i);
    }
    rfactor_axes.emplace_back(axis_i);
  }

  if (reduction_dims == rfactor_axes.size()) {
    return ir_utils::rfactorHelper(reference_tv, rfactor_axes_no_unswitch);
  }

  return ir_utils::rfactorHelper(reference_tv, rfactor_axes);
}

std::vector<TensorView*> projectPersistentBuffers(Fusion* fusion) {
  auto persistent_info = scheduler_utils::persistentBuffers(fusion);
  std::vector<TensorView*> dummy_outputs;

  // Convenience accessors
  const auto& persistent_buffers = persistent_info.persistent_buffers;
  const auto& persistent_resolution_points =
      persistent_info.persistent_buffer_resolution_points;
  const auto& projected_buffers =
      persistent_info.projectable_persistent_buffers;

  TORCH_INTERNAL_ASSERT(
      persistent_buffers.size() == persistent_resolution_points.size());

  // Iterate through projected buffers, tracking which index it corresponds too
  // since there's a resolution point entry for every buffer.
  for (auto buffer_i : c10::irange(persistent_buffers.size())) {
    auto buffer = persistent_buffers[buffer_i];
    if (std::find(projected_buffers.begin(), projected_buffers.end(), buffer) ==
        projected_buffers.end()) {
      continue;
    }

    auto resolution_points = persistent_resolution_points[buffer_i];

    std::vector<Val*> persistent_use_of_buffer;

    // Go through the resolution points one by one. Resolution points are points
    // in which the reduction branch meets the residual branch. These are points
    // where the persitent buffer may no longer be needed (one point could be
    // after another, and the buffer would be needed until the last resolution
    // points)
    for (auto resolution_point : resolution_points) {
      // Need to go through all paths from the persistent buffer to the
      // resolution point
      auto chains_to_resolution =
          DependencyCheck::getAllDependencyChains(buffer, resolution_point);
      for (auto chain : chains_to_resolution) {
        auto tv_chain = ir_utils::filterByType<TensorView>(chain);

        // To move the persistent buffers to the inputs, we need to recompute
        // the persistent buffer for all branches that don't go through a
        // reduction. If there's a reduction on the current path between the
        // persistent buffer and resolution, continue, there's no need to
        // replicate this use.
        if (std::any_of(tv_chain.begin(), tv_chain.end(), [](TensorView* tv) {
              return tv->hasReduction();
            })) {
          continue;
        }

        // Grab use of the buffer, chain[0] is the persistent buffer, chain[1]
        // is its first use.
        auto use = chain[1];

        // Only grab unique uses, a persistent buffer could be used multiple
        // times in the same expression.
        if (std::find(
                persistent_use_of_buffer.begin(),
                persistent_use_of_buffer.end(),
                use) != persistent_use_of_buffer.end()) {
          continue;
        }
        persistent_use_of_buffer.emplace_back(use);
      }
    }

    // For all uses that do not go towards the reduction operations in the
    // persistent section of the graph, recompute the persistent buffer.
    for (auto use : persistent_use_of_buffer) {
      TORCH_INTERNAL_ASSERT(use->definition() != nullptr);
      auto buffer_replicate = RecomputeTv::recompute(buffer);
      // Create a shortcut buffer <--> buffer_replicate for propagation.
      // Why is this needed?
      // Consider that we have a fusion
      //
      //   T0[I]
      //   T1[b b I] = broadcast(T0)
      //   T2[b b r] = reduction(T1)
      //   T3[b b b] = broadcast(T2)
      //   T4[b, b, I] = T1 + T3
      //   T5[b, b, r] = reduction(T4)
      //
      // After projection, it becomes
      //
      //   T0[I]
      //   T1[b b I] = broadcast(T0)
      //   T2[b b r] = reduction(T1)
      //   T3[b b b] = broadcast(T2)
      //   T6[b b I] = broadcast(T0)
      //   T4[b, b, I] = T6 + T3
      //   T5[b, b, r] = reduction(T4)
      //
      // During schedule, we need to propagate from T2 to T5. However, in the
      // resulting DAG, neither the propagation path T2->T3->T4->T5 nor
      // T2->T1->T0->T6->T4->T5 works because they both have missing root
      // domain. But adding `T7 = T1 + T6` creates a new propagation path
      // `T2->T1->T7->T6->T4->T5` which has all root domain information.
      // See FusionBroadcastPersistentReduction_CUDA for an example
      dummy_outputs.emplace_back(add(buffer_replicate, buffer));
      ir_utils::replaceValInExpr(use->definition(), buffer, buffer_replicate);
    }
  }
  return dummy_outputs;
}

} // namespace reduction_scheduler_utils
} // namespace nvfuser
