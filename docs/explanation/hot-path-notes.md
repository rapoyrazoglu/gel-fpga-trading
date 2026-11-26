# Hot-path notes and open work

Notes from reading this code against three Meeting C++ 2026 talks: one on
making libraries usable, one on low-latency techniques in a trading system,
and one on RCU and memory ordering. Each item says what the talk argued, what
this repository does, and what is still open.

## Repository shape

**The argument.** Technical quality does not help if people cannot find,
build, understand or contribute to a project: a README is a pitch, not the
documentation; main must stay green; documentation needs tutorials and
explanations as well as reference; a build must be consumable, not just
compilable.

**Here.** This restructure follows that list: one-command build from the root,
CI at the root, documentation split by purpose, a contributing guide with
starter tasks.

**Open.** No tagged release yet. No dev container. Not installable with
`find_package`.

## Hash maps on the per-order path

**The argument.** Identify instruments by a dense index and store per-instrument
state in a flat array; a hash map lookup is "O(1)" but far from free.

**Here.** Symbols already are dense indices, and symbol metadata and last
prices are flat arrays. But the risk check looks up position and limits in
`std::unordered_map`, keyed by strategy, account and symbol. The first order
for a new key allocates, inside a function marked `noexcept`.

**Open.** Replace both maps with preallocated flat storage. Measure first:
the telemetry hops give before-and-after numbers.

## Strings and allocation when sending

**The argument.** Passing `std::string` by value through layers costs more than
it looks; build messages as spans of string views and send them with
scatter-gather I/O.

**Here.** The FPGA order path has no strings at all. The CPU broker path does:
it builds a JSON document and a URL per order.

**Open, deliberately.** That path talks REST to a retail broker, where the
network round trip is milliseconds. Not worth the complexity.

## False sharing

**The argument.** Put data written by one thread and data read by others on
separate cache lines.

**Here.** Ring indices are on separate cache lines, and wire structs are
cache-line sized. Both rings now assert at compile time that their index
atomics are lock-free.

**Open.** The engine drains all rings on one thread today. If strategies move
to their own threads, the array of last-price atomics in the risk engine
becomes shared between writers and readers and should be revisited.

## Threading model

**The argument.** Pin each stateful client to one thread and feed it through
its own queue; mutual exclusion then comes from the structure, not from locks,
and it scales past the point where a shared mutex-guarded queue stops.

**Here.** The per-strategy single-producer single-consumer rings are already
that shape. What is missing is the threads: the engine is single-threaded and
does no CPU pinning.

**Open.** A design decision, not a fix.

## Memory ordering

**The argument.** Acquire and release are enough only when the algorithm needs
"what I published is visible to whoever sees my index". The moment one thread
stores a value and then reads another thread's state to decide something, that
is no longer enough, and `seq_cst` on one side does not rescue it either.

**Here.** The rings are the first kind: a slot is written, then the index is
published with release; the other side loads the index with acquire, then
reads the slot. Nothing stores and then checks the other side. The regime
mailbox and the kill switches are single atomic flags with no dependent data.

**Open.** There is no model-checking or sanitizer run for the rings. A
ThreadSanitizer job in CI would be a cheap first step.

## Mapped atomics

`ShmRing` places `std::atomic` objects in a file mapped by two processes. The
standard does not promise that works. It does on the supported targets because
a lock-free atomic is a plain load or store, which the new compile-time check
enforces. The file header still has no magic number or version, so two
processes built with different payload types would corrupt each other
silently.
