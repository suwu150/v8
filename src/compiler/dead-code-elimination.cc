// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/dead-code-elimination.h"

#include "src/compiler/common-operator.h"
#include "src/compiler/graph.h"
#include "src/compiler/js-operator.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/operator-properties.h"

namespace v8 {
namespace internal {
namespace compiler {

DeadCodeElimination::DeadCodeElimination(Editor* editor, Graph* graph,
                                         CommonOperatorBuilder* common,
                                         Zone* temp_zone)
    : AdvancedReducer(editor),
      graph_(graph),
      common_(common),
      dead_(graph->NewNode(common->Dead())),
      dead_value_(graph->NewNode(common->DeadValue())),
      zone_(temp_zone) {
  NodeProperties::SetType(dead_, Type::None());
  NodeProperties::SetType(dead_value_, Type::None());
}

namespace {

// True if we can guarantee that {node} will never actually produce a value or
// effect.
bool NoReturn(Node* node) {
  return node->opcode() == IrOpcode::kDead ||
         node->opcode() == IrOpcode::kUnreachable ||
         node->opcode() == IrOpcode::kDeadValue ||
         !NodeProperties::GetTypeOrAny(node)->IsInhabited();
}

bool HasDeadInput(Node* node) {
  for (Node* input : node->inputs()) {
    if (NoReturn(input)) return true;
  }
  return false;
}

}  // namespace

Reduction DeadCodeElimination::Reduce(Node* node) {
  switch (node->opcode()) {
    case IrOpcode::kEnd:
      return ReduceEnd(node);
    case IrOpcode::kLoop:
    case IrOpcode::kMerge:
      return ReduceLoopOrMerge(node);
    case IrOpcode::kLoopExit:
      return ReduceLoopExit(node);
    case IrOpcode::kUnreachable:
    case IrOpcode::kIfException:
      return ReduceUnreachableOrIfException(node);
    case IrOpcode::kPhi:
      return ReducePhi(node);
    case IrOpcode::kEffectPhi:
      return PropagateDeadControl(node);
    case IrOpcode::kDeoptimize:
    case IrOpcode::kReturn:
    case IrOpcode::kTerminate:
      return ReduceDeoptimizeOrReturnOrTerminate(node);
    case IrOpcode::kThrow:
      return PropagateDeadControl(node);
    case IrOpcode::kBranch:
    case IrOpcode::kSwitch:
      return ReduceBranchOrSwitch(node);
    default:
      return ReduceNode(node);
  }
  UNREACHABLE();
}

Reduction DeadCodeElimination::PropagateDeadControl(Node* node) {
  DCHECK_EQ(1, node->op()->ControlInputCount());
  Node* control = NodeProperties::GetControlInput(node);
  if (control->opcode() == IrOpcode::kDead) return Replace(control);
  return NoChange();
}

Reduction DeadCodeElimination::ReduceEnd(Node* node) {
  DCHECK_EQ(IrOpcode::kEnd, node->opcode());
  Node::Inputs inputs = node->inputs();
  DCHECK_LE(1, inputs.count());
  int live_input_count = 0;
  for (int i = 0; i < inputs.count(); ++i) {
    Node* const input = inputs[i];
    // Skip dead inputs.
    if (input->opcode() == IrOpcode::kDead) continue;
    // Compact live inputs.
    if (i != live_input_count) node->ReplaceInput(live_input_count, input);
    ++live_input_count;
  }
  if (live_input_count == 0) {
    return Replace(dead());
  } else if (live_input_count < inputs.count()) {
    node->TrimInputCount(live_input_count);
    NodeProperties::ChangeOp(node, common()->End(live_input_count));
    return Changed(node);
  }
  DCHECK_EQ(inputs.count(), live_input_count);
  return NoChange();
}


Reduction DeadCodeElimination::ReduceLoopOrMerge(Node* node) {
  DCHECK(IrOpcode::IsMergeOpcode(node->opcode()));
  Node::Inputs inputs = node->inputs();
  DCHECK_LE(1, inputs.count());
  // Count the number of live inputs to {node} and compact them on the fly, also
  // compacting the inputs of the associated {Phi} and {EffectPhi} uses at the
  // same time.  We consider {Loop}s dead even if only the first control input
  // is dead.
  int live_input_count = 0;
  if (node->opcode() != IrOpcode::kLoop ||
      node->InputAt(0)->opcode() != IrOpcode::kDead) {
    for (int i = 0; i < inputs.count(); ++i) {
      Node* const input = inputs[i];
      // Skip dead inputs.
      if (input->opcode() == IrOpcode::kDead) continue;
      // Compact live inputs.
      if (live_input_count != i) {
        node->ReplaceInput(live_input_count, input);
        for (Node* const use : node->uses()) {
          if (NodeProperties::IsPhi(use)) {
            DCHECK_EQ(inputs.count() + 1, use->InputCount());
            use->ReplaceInput(live_input_count, use->InputAt(i));
          }
        }
      }
      ++live_input_count;
    }
  }
  if (live_input_count == 0) {
    return Replace(dead());
  } else if (live_input_count == 1) {
    // Due to compaction above, the live input is at offset 0.
    for (Node* const use : node->uses()) {
      if (NodeProperties::IsPhi(use)) {
        Replace(use, use->InputAt(0));
      } else if (use->opcode() == IrOpcode::kLoopExit &&
                 use->InputAt(1) == node) {
        RemoveLoopExit(use);
      } else if (use->opcode() == IrOpcode::kTerminate) {
        DCHECK_EQ(IrOpcode::kLoop, node->opcode());
        Replace(use, dead());
      }
    }
    return Replace(node->InputAt(0));
  }
  DCHECK_LE(2, live_input_count);
  DCHECK_LE(live_input_count, inputs.count());
  // Trim input count for the {Merge} or {Loop} node.
  if (live_input_count < inputs.count()) {
    // Trim input counts for all phi uses and revisit them.
    for (Node* const use : node->uses()) {
      if (NodeProperties::IsPhi(use)) {
        use->ReplaceInput(live_input_count, node);
        TrimMergeOrPhi(use, live_input_count);
        Revisit(use);
      }
    }
    TrimMergeOrPhi(node, live_input_count);
    return Changed(node);
  }
  return NoChange();
}

Reduction DeadCodeElimination::RemoveLoopExit(Node* node) {
  DCHECK_EQ(IrOpcode::kLoopExit, node->opcode());
  for (Node* const use : node->uses()) {
    if (use->opcode() == IrOpcode::kLoopExitValue ||
        use->opcode() == IrOpcode::kLoopExitEffect) {
      Replace(use, use->InputAt(0));
    }
  }
  Node* control = NodeProperties::GetControlInput(node, 0);
  Replace(node, control);
  return Replace(control);
}

Reduction DeadCodeElimination::ReduceNode(Node* node) {
  DCHECK(!IrOpcode::IsGraphTerminator(node->opcode()));
  int const effect_input_count = node->op()->EffectInputCount();
  int const control_input_count = node->op()->ControlInputCount();
  DCHECK_LE(control_input_count, 1);
  if (control_input_count == 1) {
    Reduction reduction = PropagateDeadControl(node);
    if (reduction.Changed()) return reduction;
  }
  if (effect_input_count == 0 &&
      (control_input_count == 0 || node->op()->ControlOutputCount() == 0)) {
    return ReducePureNode(node);
  }
  if (effect_input_count > 0) {
    return ReduceEffectNode(node);
  }
  return NoChange();
}

Reduction DeadCodeElimination::ReducePhi(Node* node) {
  DCHECK_EQ(IrOpcode::kPhi, node->opcode());
  Reduction reduction = PropagateDeadControl(node);
  if (reduction.Changed()) return reduction;
  if (PhiRepresentationOf(node->op()) == MachineRepresentation::kNone ||
      !NodeProperties::GetTypeOrAny(node)->IsInhabited()) {
    return Replace(dead_value());
  }
  return NoChange();
}

Reduction DeadCodeElimination::ReducePureNode(Node* node) {
  DCHECK_EQ(0, node->op()->EffectInputCount());
  int input_count = node->op()->ValueInputCount();
  for (int i = 0; i < input_count; ++i) {
    Node* input = NodeProperties::GetValueInput(node, i);
    if (NoReturn(input)) {
      return Replace(dead_value());
    }
  }
  return NoChange();
}

Reduction DeadCodeElimination::ReduceUnreachableOrIfException(Node* node) {
  DCHECK(node->opcode() == IrOpcode::kUnreachable ||
         node->opcode() == IrOpcode::kIfException);
  Reduction reduction = PropagateDeadControl(node);
  if (reduction.Changed()) return reduction;
  Node* effect = NodeProperties::GetEffectInput(node, 0);
  if (effect->opcode() == IrOpcode::kDead) {
    return Replace(effect);
  }
  if (effect->opcode() == IrOpcode::kUnreachable) {
    RelaxEffectsAndControls(node);
    return Replace(dead_value());
  }
  return NoChange();
}

Reduction DeadCodeElimination::ReduceEffectNode(Node* node) {
  DCHECK_EQ(1, node->op()->EffectInputCount());
  Node* effect = NodeProperties::GetEffectInput(node, 0);
  if (effect->opcode() == IrOpcode::kDead) {
    return Replace(effect);
  }
  if (HasDeadInput(node)) {
    if (effect->opcode() == IrOpcode::kUnreachable) {
      RelaxEffectsAndControls(node);
      return Replace(dead_value());
    }

    Node* control = node->op()->ControlInputCount() == 1
                        ? NodeProperties::GetControlInput(node, 0)
                        : graph()->start();
    Node* unreachable =
        graph()->NewNode(common()->Unreachable(), effect, control);
    ReplaceWithValue(node, dead_value(), node, control);
    return Replace(unreachable);
  }

  return NoChange();
}

Reduction DeadCodeElimination::ReduceDeoptimizeOrReturnOrTerminate(Node* node) {
  DCHECK(node->opcode() == IrOpcode::kDeoptimize ||
         node->opcode() == IrOpcode::kReturn ||
         node->opcode() == IrOpcode::kTerminate);
  Reduction reduction = PropagateDeadControl(node);
  if (reduction.Changed()) return reduction;
  if (HasDeadInput(node)) {
    Node* effect = NodeProperties::GetEffectInput(node, 0);
    Node* control = NodeProperties::GetControlInput(node, 0);
    if (effect->opcode() != IrOpcode::kUnreachable) {
      effect = graph()->NewNode(common()->Unreachable(), effect, control);
    }
    node->TrimInputCount(2);
    node->ReplaceInput(0, effect);
    node->ReplaceInput(1, control);
    NodeProperties::ChangeOp(node, common()->Throw());
    return Changed(node);
  }
  return NoChange();
}

Reduction DeadCodeElimination::ReduceLoopExit(Node* node) {
  Node* control = NodeProperties::GetControlInput(node, 0);
  Node* loop = NodeProperties::GetControlInput(node, 1);
  if (control->opcode() == IrOpcode::kDead ||
      loop->opcode() == IrOpcode::kDead) {
    return RemoveLoopExit(node);
  }
  return NoChange();
}

Reduction DeadCodeElimination::ReduceBranchOrSwitch(Node* node) {
  DCHECK(node->opcode() == IrOpcode::kBranch ||
         node->opcode() == IrOpcode::kSwitch);
  Reduction reduction = PropagateDeadControl(node);
  if (reduction.Changed()) return reduction;
  Node* condition = NodeProperties::GetValueInput(node, 0);
  if (condition->opcode() == IrOpcode::kDeadValue) {
    // Branches or switches on {DeadValue} must originate from unreachable code
    // and cannot matter. Due to schedule freedom between the effect and the
    // control chain, they might still appear in reachable code. Remove them by
    // always choosing the first projection.
    size_t const projection_cnt = node->op()->ControlOutputCount();
    Node** projections = zone_->NewArray<Node*>(projection_cnt);
    NodeProperties::CollectControlProjections(node, projections,
                                              projection_cnt);
    Replace(projections[0], NodeProperties::GetControlInput(node));
    return Replace(dead());
  }
  return NoChange();
}

void DeadCodeElimination::TrimMergeOrPhi(Node* node, int size) {
  const Operator* const op = common()->ResizeMergeOrPhi(node->op(), size);
  node->TrimInputCount(OperatorProperties::GetTotalInputCount(op));
  NodeProperties::ChangeOp(node, op);
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
