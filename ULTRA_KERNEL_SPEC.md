# ULTRA Kernel Specification

## Deterministic Cognitive Kernel for Code Intelligence

Version: 1.0
Scope: Layers 0–12

This document defines the **core architectural contracts** for the Ultra kernel.
All implementations must comply with this specification.

The kernel provides deterministic, incremental reasoning over evolving codebases.

---

# Design Principles

Ultra must satisfy the following guarantees:

Deterministic
Snapshot-bound reasoning
Sparse dependency graph optimization
Incremental computation
Bounded memory growth
Token-efficient context extraction
Model-agnostic operation

Ultra is **not an IDE**, **not a workflow engine**, and **not a cloud platform**.

It is a **cognitive kernel for code understanding and reasoning**.

---

# Graph Model

Ultra represents codebases as a dependency graph.

Nodes:

* File nodes
* Symbol nodes

Edges:

* File dependencies
* Symbol dependencies
* Signature relationships

The graph must be **sparse and immutable**.

Sparse property:

Average node degree must remain low (<5) for large repositories.

All algorithms must scale with:

O(k)

Where k = affected nodes.

Never scale with full repository size.

---

# Snapshot Model

All reasoning must occur on immutable snapshots.

```
struct GraphSnapshot {
    BaseGraph* base;
    Overlay* overlay;
    uint64_t version;
};
```

Snapshots guarantee:

* deterministic reads
* lock-free traversal
* branch isolation

Snapshots must never mutate after creation.

---

# Layer Specifications

## Layer 0 — Event & Version Controller

Responsibilities:

* File system watcher
* Change event queue
* Git branch tracking
* Commit tracking
* Version counter

Requirements:

Single-threaded deterministic ordering.

All mutations originate from this layer.

Events include:

FILE_MODIFIED
FILE_CREATED
FILE_DELETED
BRANCH_SWITCH
COMMIT_UPDATE

Each event increments the global version counter.

---

## Layer 1 — Parallel Hash Engine

Purpose:

Detect file changes without full rescans.

Requirements:

* Thread pool (2–4 threads)
* SHA-256 hashing
* Thread-local buffers
* Zero-copy file reads where possible

Hash operations must run in parallel but remain bounded.

Hash reuse must be supported for unchanged files.

---

## Layer 2 — Semantic Parsing Engine

Purpose:

Extract structural information from source code.

Regex parsing is prohibited.

Required technologies:

Tree-sitter
or
Clang AST

Extracted data:

Symbol nodes
Function signatures
Class definitions
Dependency edges

The engine must not retain full ASTs after extraction.

Memory must be freed after graph merge.

---

## Layer 3 — Immutable Graph + Copy-On-Write Overlays

Base Graph:

Immutable contiguous arrays.

Memory layout:

SymbolNode[]
EdgeStorage[]
FileNode[]

Overlay Graph:

Contains only modified nodes.

Overlay structure:

Modified symbols
Edge delta arrays
Removed node list

Branch overlays must support atomic snapshot swaps.

---

## Layer 4 — Epoch-Based Reclamation

Purpose:

Safe memory reclamation without locking.

Required components:

Global epoch counter
Thread-local read epochs
Retire lists
RAII read guards

Guarantees:

No use-after-free
No lock contention
Safe overlay deletion

Memory must remain bounded.

---

## Layer 5 — Branch Cognitive Cache

Purpose:

Keep recently used branch overlays active.

Requirements:

2–3 overlays maximum.

Eviction policy:

LRU + usage score.

Overlay arenas must be reclaimed using the epoch system.

Base graph must never be duplicated.

---

## Layer 6 — Sub-Conscious Working Memory (Hot Slice)

Purpose:

Fast retrieval of highly relevant nodes.

Stores:

NodeID
BranchID
Version
Relevance score

HotSlice must only store NodeIDs.

Symbol data must remain in the graph.

Eviction policy:

Least relevant nodes removed when capacity exceeded.

---

## Layer 7 — Relevance Calibration Engine

Purpose:

Adaptive relevance scoring.

Signals tracked:

AI query frequency
Developer edit frequency
Impact traversal frequency
Context injection frequency

Weights:

Recency
Centrality
Change rate
Dependency proximity

The engine must self-adjust relevance weights over time.

---

## Layer 8 — Impact Radius Engine

Purpose:

Predict consequences of code changes.

Traversal rules:

Depth-limited traversal
Sparse edge iteration
Parallel traversal for large regions

Output:

Affected node set
Impact depth score

---

## Layer 9 — Cross-Branch Diff Engine

Purpose:

Detect semantic changes across branches.

Diff types:

Symbol diff
Signature diff
Dependency diff

Risk classification:

BODY_CHANGE
SIGNATURE_CHANGE
DEPENDENCY_CHANGE
API_REMOVAL

---

## Layer 10 — Precision Invalidation Engine

Purpose:

Invalidate only affected graph regions.

Rules:

Depth-limited recompute
Rename-aware updates
Lazy HotSlice invalidation

Full rebuild allowed only when thresholds exceeded.

---

## Layer 11 — Context Compression Engine

Purpose:

Generate minimal AI context slices.

Requirements:

Deterministic output
Stable node ordering
Token budgeting

Context output format must be JSON.

The same snapshot must always produce identical context.

---

## Layer 12 — CPU Governor

Purpose:

Maintain lightweight runtime.

Responsibilities:

Adaptive thread scaling
Background cleanup
Idle memory reclamation

The governor must ensure Ultra does not overwhelm system resources.

---

# Performance Targets

Large repository support:

1 million LOC minimum.

Index build time target:

<30 seconds for medium repositories.

Incremental update latency:

<200 milliseconds for single file changes.

Context generation latency:

<50 milliseconds.

---

# Development Rules

No layer may violate the contracts of a lower layer.

Layer dependencies must remain strictly downward.

Example:

Layer 8 may depend on Layer 3.

Layer 3 must not depend on Layer 8.

---

# Non Goals

Ultra will not:

Replace IDEs
Replace CI systems
Replace container orchestration
Act as a distributed platform

Ultra focuses solely on **deterministic cognitive reasoning for codebases**.
