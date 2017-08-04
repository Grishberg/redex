/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Liveness.h"
#include "RegisterType.h"
#include "Transform.h"

class IRCode;

namespace regalloc {

using reg_t = uint16_t;

/*
 * Tracks which instructions that can be encoded in range form should take
 * that form.
 *
 * It's essentially just a set that tracks insertion order (so we can
 * allocate these instructions in a deterministic fashion.)
 */
class RangeSet {
 public:
  void emplace(const IRInstruction* insn) {
    if (contains(insn)) {
      return;
    }
    m_range_set.emplace(const_cast<IRInstruction*>(insn));
    m_range_vec.emplace_back(const_cast<IRInstruction*>(insn));
  }
  bool contains(const IRInstruction* insn) const {
    return m_range_set.find(const_cast<IRInstruction*>(insn)) !=
           m_range_set.end();
  }
  std::vector<IRInstruction*>::iterator begin() { return m_range_vec.begin(); }
  std::vector<IRInstruction*>::iterator end() { return m_range_vec.end(); }
  std::vector<IRInstruction*>::const_iterator begin() const {
    return m_range_vec.begin();
  }
  std::vector<IRInstruction*>::const_iterator end() const {
    return m_range_vec.end();
  }
  size_t size() const { return m_range_vec.size(); }

 private:
  std::vector<IRInstruction*> m_range_vec;
  std::unordered_set<IRInstruction*> m_range_set;
};

namespace interference {

namespace impl {

class GraphBuilder;

template <typename T>
struct OrderedPair {
  T first;
  T second;

  OrderedPair(T u, T v)
      : first(std::min(u, v)), second(std::max(u, v)) {}

  bool operator==(const OrderedPair& that) const {
    return first == that.first && second == that.second;
  }
};

template <typename T>
inline size_t hash_value(const OrderedPair<T>& pair) {
  size_t seed{0};
  boost::hash_combine(seed, pair.first);
  boost::hash_combine(seed, pair.second);
  return seed;
}

} // namespace impl

class Node {
 public:
  Node() { m_props.set(ACTIVE); }

  uint8_t width() const { return m_width; }

  /*
   * Whether this node corresponds to the short live range generated by a
   * spill. We don't want to re-spill these ranges.
   */
  bool is_spilt() const { return m_props[SPILL]; }

  /*
   * Nodes become inactive when they are coalesced or taken out of the graph
   * during simplification.
   */
  bool is_active() const { return m_props[ACTIVE]; }

  bool is_param() const { return m_props[PARAM]; }

  /*
   * Whether this register is ever used by a range instruction.
   */
  bool is_range() const { return m_props[RANGE]; }

  uint32_t weight() const { return m_weight; }

  uint32_t colorable_limit() const;

  bool definitely_colorable() const;

  /*
   * The maximum vreg this node can be mapped to without spilling. Since
   * different opcodes have different maximums, this ends up being a per-node
   * value instead of a global value.
   */
  reg_t max_vreg() const { return m_max_vreg; }

  /*
   * The register allocator assumes that every live range has exactly one
   * RegisterType (and that type cannot be CONFLICT). This is more restrictive
   * than what the dexopt verifier requires, but dx generates code that
   * conforms to this restriction, and it would complicate our allocator to
   * handle code that didn't. For example, the following code should verify,
   * but fails our requirement:
   *
   *   const/4 v0, 0 # v0 => RegisterType::ZERO
   *   if-eqz v1
   *   if-true-branch:
   *   add-int v0, v0, v0 # v0 => RegisterType::NORMAL
   *   if-false-branch:
   *   invoke-static v0 LFoo;.bar(LBar;) # v0 => RegisterType::OBJECT
   */
  RegisterType type() const { return m_type_domain.element(); }

  const std::vector<reg_t>& adjacent() const { return m_adjacent; }

  enum Property {
    PARAM,
    RANGE,
    SPILL,
    ACTIVE,
    PROPS_SIZE
  };

 private:
  uint32_t m_weight{0};
  reg_t m_max_vreg{max_unsigned_value(16)};
  // While the width is implicit in the register type, looking up the type to
  // determine the width is a little more expensive than storing the width
  // directly. Since the width() function is quite hot, it's worth optimizing.
  uint8_t m_width{0};
  std::bitset<PROPS_SIZE> m_props;
  RegisterTypeDomain m_type_domain{RegisterType::UNKNOWN};
  std::vector<reg_t> m_adjacent;

  friend class Graph;
  friend class impl::GraphBuilder;
};

class Graph {
  struct ActiveFilter {
    bool operator()(const std::pair<reg_t, Node>& pair) {
      return pair.second.is_active();
    }
  };

 public:
  const Node& get_node(reg_t) const;

  const std::unordered_map<reg_t, Node>& nodes() const { return m_nodes; }

  std::unordered_map<reg_t, Node>& nodes() { return m_nodes; }

  boost::filtered_range<ActiveFilter, const std::unordered_map<reg_t, Node>>
  active_nodes() const {
    return boost::adaptors::filter(m_nodes, ActiveFilter());
  }

  bool is_adjacent(reg_t u, reg_t v) const {
    return m_adj_matrix.find(Edge(u, v)) != m_adj_matrix.end();
  }

  bool is_coalesceable(reg_t u, reg_t v) const {
    return !is_adjacent(u, v) || !m_adj_matrix.at(Edge(u, v));
  }

  bool has_containment_edge(reg_t u, reg_t v) const {
    return m_containment_graph.find(ContainmentEdge(u, v)) !=
           m_containment_graph.end();
  }

  /*
   * Returns the live-out info for a given instruction that has a potential
   * range encoding. We can use it to make better allocation decisions for
   * these instructions.
   */
  const LivenessDomain& get_liveness(const IRInstruction* insn) const {
    return m_range_liveness.at(const_cast<IRInstruction*>(insn));
  }

  void remove_node(reg_t);

  /*
   * Combines v into u. Gives u all of v's neighbors and marks v as inactive.
   */
  void combine(reg_t u, reg_t v);

  /*
   * Print the graph in the DOT graph description language.
   */
  std::ostream& write_dot_format(std::ostream&) const;

 private:
  using ContainmentEdge = std::pair<reg_t, reg_t>;
  Graph() = default;
  void add_edge(reg_t, reg_t, bool can_coalesce = false);
  void add_coalesceable_edge(reg_t u, reg_t v) { add_edge(u, v, true); }
  void add_containment_edge(reg_t u, reg_t v) {
    if (u == v) {
      return;
    }
    m_containment_graph.emplace(ContainmentEdge(u, v));
  }

  std::unordered_map<reg_t, Node> m_nodes;
  using Edge = impl::OrderedPair<reg_t>;
  std::unordered_map<Edge, bool /* not_coalesceable */, boost::hash<Edge>>
      m_adj_matrix;
  std::unordered_set<ContainmentEdge, boost::hash<ContainmentEdge>>
      m_containment_graph;
  // This map contains the LivenessDomains for all instructions which could
  // potentialy take on the /range format.
  std::unordered_map<IRInstruction*, LivenessDomain> m_range_liveness;

  friend class impl::GraphBuilder;
};

namespace impl {

/* Returns ⌈a/b⌉ */
inline uint32_t div_ceil(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

/*
 * This class is a friend of Graph and Node. It allows them to expose a more
 * limited public interface.
 */
class GraphBuilder {
  static void update_node_constraints(const IRInstruction*,
                                      const RangeSet&,
                                      Graph*);

 public:
  static Graph build(IRCode*, reg_t initial_regs, const RangeSet&);

  // For unit tests
  static Graph create_empty() { return Graph(); }
  static void make_node(Graph*, reg_t, RegisterType, reg_t max_vreg);
  static void add_edge(Graph*, reg_t, reg_t);
};

uint32_t edge_weight(uint8_t, uint8_t);

} // namespace impl

IRInstruction* find_check_cast(const MethodItemEntry& mie);

inline Graph build_graph(IRCode* code,
                         reg_t initial_regs,
                         const RangeSet& range_set) {
  return impl::GraphBuilder::build(code, initial_regs, range_set);
}

} // interference

} // namespace regalloc
