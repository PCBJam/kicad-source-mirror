/*

    auto-generated file, do not modify!
    libcontext - a slightly more portable version of boost::context
    Copyright Martin Husemann 2013.
    Copyright Oliver Kowalke 2009.
    Copyright Sergue E. Leontiev 2013
    Copyright Thomas Sailer 2013.
    Minor modifications by Tomasz Wlostowski 2016.

 Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE.BOOSTv1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)

*/
#include <csetjmp>
#include <cstdlib>
#include <libcontext.h>

// WASM/Emscripten: implement libcontext using Emscripten fibers so KiCad's
// existing coroutine machinery can switch stacks on the web build too.
//
// Phase A of pcbjam docs/features/async/22: this backend is a THIN ADAPTER
// over the scheduler registry (wx/wasm/private/sched_context.h, header-only).
// The registry owns every emscripten_fiber_t, its asyncify buffer, and the
// answer to "is this target safe to enter?"; this file keeps libcontext's
// protocol exactly as it was — symmetric swap, the INVOCATION_ARGS contract,
// ghost-resume epochs, refcounting — and keeps its old guards as cross-check
// tripwires that must agree with the registry (a disagreement beacons).
#if defined(LIBCONTEXT_PLATFORM_wasm32)
#include <emscripten.h>

#include <emscripten/fiber.h>
#include <wx/wasm/private/sched_context.h>

#include <cstring>
#include <new>

namespace libcontext {

// The handlesleep runtime shim maintains Asyncify.__wakingRoot: it is >0
// exactly while the MAIN context's own sleep-wake continuation is executing
// synchronously (rewind + resumed forward run, until its next park). A fiber
// swap OUT of main inside that extent writes a suspension that is broken by
// construction: emscripten_fiber_swap records the rewind entry as the BOTTOM
// of Asyncify.exportCallStack — the re-invoked main export — while the
// asyncify capture only spans the frames near the swap site (measured 1.3KB
// vs the ~10KB of a valid fresh-entry capture, 2026-08-03 local repro). The
// eventual rewind of that pairing traps immediately ("unreachable executed" /
// "index out of bounds"), no matter when it runs — deferring it changes
// nothing (proven: a microtask-deferred retry on an empty stack trapped
// identically). Prevention is the only option, and it must happen BEFORE
// _asyncify_start_unwind writes the doomed data — i.e. here, not in the JS
// finishContextSwitch layer.
EM_JS( int, wasm_root_wake_in_flight, (), {
    return ( typeof Asyncify !== "undefined" && ( Asyncify.__wakingRoot | 0 ) > 0 ) ? 1 : 0;
} );

namespace
{

// 512K (was 64K): an asyncify unwind saves every live frame's locals into
// this per-fiber buffer. Shallow tool-loop yields fit 64K, but a DEEP park —
// e.g. a collab apply suspending inside commit.Push → connectivity → font
// work — overflowed it, and with assertions compiled out the overflow
// silently corrupted the saved rewind state: the rewind then re-dispatched
// indirect call sites from garbage locals ("table index is out of bounds" on
// doRewind — drift-trio finding #10b, standalone-hardening 0008 §10).
constexpr size_t ASYNCIFY_STACK_SIZE = 512 * 1024;
[[maybe_unused]] constexpr int MAX_WASM_FCONTEXT_LOGS = 220;

struct invocation_args_probe
{
    int   type;
    void* destination;
    void* context;
};


const char* invocation_type_name( int aType )
{
    switch( aType )
    {
    case 0:
        return "FROM_ROOT";

    case 1:
        return "FROM_ROUTINE";

    case 2:
        return "CONTINUE_AFTER_ROOT";

    default:
        return "UNKNOWN";
    }
}


[[maybe_unused]] const char* describe_transfer_value( intptr_t aValue, char* aBuffer, size_t aBufferSize )
{
    if( !aValue || aValue < 0x10000 )
    {
        std::snprintf( aBuffer, aBufferSize, "raw=%p", reinterpret_cast<void*>( aValue ) );
        return aBuffer;
    }

    auto* probe = reinterpret_cast<const invocation_args_probe*>( aValue );

    std::snprintf( aBuffer, aBufferSize,
                   "raw=%p type=%s(%d) dest=%p ctx=%p",
                   reinterpret_cast<void*>( aValue ),
                   invocation_type_name( probe->type ),
                   probe->type,
                   probe->destination,
                   probe->context );
    return aBuffer;
}


struct wasm_fcontext
{
    // Phase A: the emscripten_fiber_t and its asyncify buffer moved into the
    // scheduler registry (sched_id names them there); this struct keeps only
    // libcontext's PROTOCOL state.
    pcbjam_sched::ContextId sched_id = 0;
    void (* entry )( intptr_t ) = nullptr;
    wasm_fcontext* return_to = nullptr;
    intptr_t transfer_value = 0;
    bool initialized = false;
    bool running = false;
    bool finished = false;
    // True ONLY between a real fiber_swap suspension (its fiber rewind data
    // is valid) and the next swap-in. A context whose flag is false is
    // mid-execution — possibly asyncify-parked inside handleSleep below a JS
    // turn, a wasm-only state its fiber data knows nothing about. Swapping
    // into such a context rewinds STALE data (finishContextSwitch → doRewind
    // → "unreachable executed" — the 2026-07 production board-load trap) and
    // poisons every later Asyncify entry ("index out of bounds").
    //
    // Phase A keeps this AUTHORITATIVE (see jump_fcontext): the registry
    // observes and beacons disagreements, but must not refuse what this
    // permits. Ownership of the answer moves at Phase B; the flag dies at F.
    bool swap_suspended = false;
    bool releasable = true;
    // On the deferred-release list (see zombie_contexts below): the last
    // refcount dropped while the fiber was still logically alive.
    bool zombie = false;
    int refcount = 0;
    uint32_t resume_epoch = 0;
    uint32_t id = 0;
};


wasm_fcontext* g_current_context = nullptr;
wasm_fcontext g_main_context;
bool g_main_initialized = false;
// The MAIN STACK's fiber-lane identity, adopted lazily the first time a jump
// happens while no scheduler context runs (see resolve_root_identity).
pcbjam_sched::ContextId g_main_stack_sched_id = 0;
[[maybe_unused]] int g_log_count = 0;
uint32_t g_next_context_id = 1;
uint32_t g_main_refresh_count = 0;


// Phase A tripwire: the legacy protocol fields (g_current_context,
// swap_suspended) and the scheduler registry are maintained in lockstep; any
// disagreement means the adapter has a hole. MUST stay silent (doc 22 §5A
// gate); deleted with the legacy fields at Phase F.
void divergence_beacon( const char* aKind, const wasm_fcontext* aOld,
                        const wasm_fcontext* aNew )
{
    static int s_divergences = 0;
    ++s_divergences;

    if( s_divergences <= 10 || s_divergences % 100 == 0 )
    {
        EM_ASM( { console.warn( "[collab-fcontext] sched-divergence-" + UTF8ToString( $0 )
                                + ": old=" + $1 + " new=" + $2
                                + " (occurrence " + $3 + ")" ); },
                aKind, (int) ( aOld ? aOld->id : 0 ), (int) ( aNew ? aNew->id : 0 ),
                s_divergences );
    }
}


// The ONE way a libcontext swap happens now: through the scheduler registry,
// which records who is on the CPU and performs the emscripten_fiber_swap. A
// refusal means the target was released while someone still held its
// fcontext pointer — previously a silent use-after-free; now a loud beacon
// and no swap, which the caller's ghost-resume contract contains (the caller
// sees an unchanged epoch and returns null INVOCATION_ARGS).
// Phase B: a swap is a STAR TRANSFER — park the source, make the target
// runnable, and let the scheduler perform the entry. The value handed back is
// whatever the next transfer into us carries, which is exactly libcontext's
// return-from-jump_fcontext contract.
//
// Falls back to Phase A's direct swap when the source is not the scheduler's
// current context: that means nothing has moved onto contexts in this build
// (the standalone coroutine harnesses), and a transfer would have nothing to
// park.
intptr_t sched_swap( wasm_fcontext* aFrom, wasm_fcontext* aTo, intptr_t aValue )
{
    if( pcbjam_sched::current() == aFrom->sched_id && pcbjam_sched::can_yield_here() )
        return pcbjam_sched::fiber_transfer( aFrom->sched_id, aTo->sched_id, aValue );

    if( !pcbjam_sched::fiber_swap( aFrom->sched_id, aTo->sched_id ) )
    {
        EM_ASM( { console.warn( "[collab-fcontext] swap-lost: scheduler refused old=" + $0
                                + " new=" + $1 ); },
                (int) aFrom->id, (int) aTo->id );
    }

    return aFrom->transfer_value;
}


[[maybe_unused]] uint32_t context_id( const wasm_fcontext* aCtx )
{
    return aCtx ? aCtx->id : 0;
}


void log_wasm_fcontext( const char* aLabel, wasm_fcontext* aCtx, wasm_fcontext* aOther,
                        intptr_t aValue, fcontext_t* aSlot = nullptr,
                        wasm_fcontext* aPrevious = nullptr )
{
#if defined( KICAD_DIAG_COROUTINE )
    if( g_log_count >= MAX_WASM_FCONTEXT_LOGS )
        return;

    ++g_log_count;

    char value_description[160];

    // stdout (not stderr) so this shows as a [KICAD_OUT] log, not an error.
    std::printf( "[WASM_FCONTEXT] %s ctx=%p[#%u] other=%p[#%u] return_to=%p[#%u] slot=%p prev=%p[#%u] "
                 "main_refresh=%u value=%s finished=%d running=%d epoch=%u\n",
                 aLabel,
                 static_cast<void*>( aCtx ),
                 context_id( aCtx ),
                 static_cast<void*>( aOther ),
                 context_id( aOther ),
                 aCtx ? static_cast<void*>( aCtx->return_to ) : nullptr,
                 aCtx ? context_id( aCtx->return_to ) : 0,
                 static_cast<void*>( aSlot ),
                 static_cast<void*>( aPrevious ),
                 context_id( aPrevious ),
                 g_main_refresh_count,
                 describe_transfer_value( aValue, value_description, sizeof( value_description ) ),
                 aCtx ? aCtx->finished : 0,
                 aCtx ? aCtx->running : 0,
                 aCtx ? aCtx->resume_epoch : 0 );
    std::fflush( stdout );
#else
    (void) aLabel;
    (void) aCtx;
    (void) aOther;
    (void) aValue;
    (void) aSlot;
    (void) aPrevious;
#endif
}


// -------------------------------------------------------------------------
// Deferred release: a GRACE RING, not a lifetime fix.
//
// KiCad jumps into coroutines whose last refcount has already been dropped —
// TOOL_MANAGER keeps raw `fcontext_t`s that outlive the COROUTINE that owned
// them. The legacy backend "supported" this by reading freed memory: the
// emscripten_fiber_t and its 512K buffer lived INSIDE the struct, so a stale
// jump swapped into a freed-but-not-yet-reused allocation and usually worked.
//
// Absorbing the backend removes that luck — the fiber now lives in the
// registry, and a freed struct means a garbage `sched_id`, so the swap is
// refused and the tool wedges (measured 2026-08-06: "click never landed a
// selection", "box-select never selected anything" — every canvas tool).
//
// WHY THE LUCK HELD FOR YEARS, exactly: the freed block was ~512K. malloc
// parks an allocation that size in a large bin and only hands it back to a
// request of similar size, so "freed" stayed readable essentially forever.
// Absorbing the buffer into the registry left a ~64-byte struct behind —
// tiny allocations are recycled on the next call, which is why the garbage
// `sched_id` (0x16554B0, a freelist pointer) appears immediately now. The
// change did not introduce the use-after-free; it removed the size accident
// that hid it.
//
// Phase A must not be the phase that fixes this, and must not be the phase
// that breaks it either. Two rules reproduce the old behaviour honestly:
//
//   1. The STRUCT is never freed. It is protocol state only now, so keeping
//      every one costs tens of bytes — cheaper than the accident it replaces,
//      and it makes a stale jump land on a real `sched_id` instead of a
//      freelist pointer.
//   2. The FIBER (512K buffer + registry entry) is what the ring bounds.
//      Eviction prefers coroutines that actually FINISHED — the churn — and
//      never takes one that is running or never completed, because those are
//      precisely the ones TOOL_MANAGER still jumps into (measured: a ring
//      that evicted by age alone dropped tool coroutine #1, and "m"-move and
//      lock-resist both failed). An evicted context keeps its struct with
//      `sched_id` zeroed, so a later jump takes the ghost contract that
//      callers already handle, rather than a refusal or a wild swap.
//
// Phase B removes the need by giving coroutines scheduler-owned lifetimes.
// Until then the eviction beacon is the measurement of how much KiCad leans
// on this.
// -------------------------------------------------------------------------
// 32 × 512K ≈ 16 MB worst case, and only if that many released coroutines are
// live at once; the steady state after a board load is a handful.
constexpr size_t GRACE_RING_CAPACITY = 32;

std::vector<wasm_fcontext*>& grace_ring()
{
    static std::vector<wasm_fcontext*> s_ring;
    return s_ring;
}


void drop_from_grace_ring( wasm_fcontext* aCtx )
{
    auto& ring = grace_ring();

    for( size_t i = 0; i < ring.size(); ++i )
    {
        if( ring[i] == aCtx )
        {
            ring.erase( ring.begin() + i );
            break;
        }
    }

    aCtx->zombie = false;
}


// A jump whose target has had its fiber reclaimed. Returns the same null
// INVOCATION_ARGS the ghost path returns, which every caller already handles.
void beacon_jump_into_reclaimed( const wasm_fcontext* aCtx )
{
    static int s_hits = 0;
    ++s_hits;

    if( s_hits <= 10 || s_hits % 100 == 0 )
    {
        EM_ASM( { console.warn( "[collab-fcontext] jump-into-reclaimed: ctx=" + $0
                                + " was evicted from the grace ring (occurrence "
                                + $1 + ")" ); },
                (int) aCtx->id, s_hits );
    }
}


// Give up the fiber, keep the struct. `sched_id = 0` is the recorded "this
// coroutine's stack is gone" state — a later jump reads it and takes the
// ghost contract instead of chasing a freelist pointer.
void reclaim_fiber( wasm_fcontext* aCtx )
{
    static int s_reclaimed = 0;
    ++s_reclaimed;

    if( s_reclaimed <= 10 || s_reclaimed % 100 == 0 )
    {
        EM_ASM( { console.warn( "[collab-fcontext] grace-ring-evict: ctx=" + $0
                                + " fiber reclaimed (occurrence " + $1 + ")" ); },
                (int) aCtx->id, s_reclaimed );
    }

    pcbjam_sched::fiber_release( aCtx->sched_id );
    aCtx->sched_id = 0;
    aCtx->swap_suspended = false;   // no stack to enter any more
}


// Is this released context safe to take the fiber from? Only a coroutine that
// ran to completion is: TOOL_MANAGER re-enters long-lived, never-finished tool
// coroutines long after their refcount hits zero, and those must keep their
// stacks.
bool evictable( const wasm_fcontext* aCtx )
{
    return aCtx->finished
           && aCtx != g_current_context
           && pcbjam_sched::status_of( aCtx->sched_id ) != pcbjam_sched::Status::Running;
}


void trim_grace_ring()
{
    auto& ring = grace_ring();

    while( ring.size() > GRACE_RING_CAPACITY )
    {
        size_t victim = ring.size();

        for( size_t i = 0; i < ring.size(); ++i )
        {
            if( evictable( ring[i] ) )
            {
                victim = i;
                break;   // oldest evictable
            }
        }

        if( victim == ring.size() )
        {
            // Everything here is still jumpable-into. Growing is the correct
            // answer — dropping one of these is what broke the move tools.
            static bool s_warned = false;

            if( !s_warned )
            {
                s_warned = true;
                EM_ASM( { console.warn( "[collab-fcontext] grace-ring-over-capacity: " + $0
                                        + " released coroutines, none evictable" ); },
                        (int) ring.size() );
            }

            return;
        }

        wasm_fcontext* ctx = ring[victim];
        ring.erase( ring.begin() + victim );
        reclaim_fiber( ctx );
    }
}


void retain_context( wasm_fcontext* aCtx )
{
    if( !aCtx || !aCtx->releasable )
        return;

    // A slot re-captured a released context: it is owned again, so it leaves
    // the ring rather than waiting to be evicted out from under its holder.
    if( aCtx->zombie )
        drop_from_grace_ring( aCtx );

    ++aCtx->refcount;
}


void release_context( wasm_fcontext* aCtx )
{
    if( !aCtx || !aCtx->releasable )
        return;

    if( aCtx->refcount <= 0 )
        return;

    --aCtx->refcount;

    if( aCtx->refcount == 0 && !aCtx->zombie )
    {
        static int s_released = 0;
        ++s_released;

        if( s_released <= 5 || s_released % 500 == 0 )
        {
            EM_ASM( { console.warn( "[collab-fcontext] release-deferred: ctx=" + $0
                                    + " enters the grace ring (occurrence " + $1 + ")" ); },
                    (int) aCtx->id, s_released );
        }

        aCtx->zombie = true;
        grace_ring().push_back( aCtx );
        trim_grace_ring();
    }
}


void assign_saved_context( fcontext_t* aSlot, wasm_fcontext* aCtx )
{
    if( !aSlot )
        return;

    auto* previous = static_cast<wasm_fcontext*>( *aSlot );
    log_wasm_fcontext( "save-slot", aCtx, g_current_context, 0, aSlot, previous );

    if( previous == aCtx )
    {
        *aSlot = aCtx;
        return;
    }

    release_context( previous );
    *aSlot = aCtx;
    retain_context( aCtx );
}


// The scheduler identity of "the root" for a jump happening RIGHT NOW.
//
// MEASURED PLAN CORRECTION (2026-08-07, nested case 3 + races pump-error).
// Doc 22 first said "the root adopts the RUNNING context, never the main
// stack" — binding it ONCE, at first jump. That is wrong while any entry
// still runs on the main stack (mailbox timers, DOM handlers — everything
// until Phase E completes): a timer jumping a fiber from the MAIN stack then
// used the DISPATCH context's fiber struct as its from-side while that
// context sat PARKED in a modal — overwriting the parked capture and marking
// it Running, so the modal's later resolve hit "mark_ready() on a running
// context" and the wake was lost.
//
// The root's identity is a PER-JUMP question: the running scheduler context
// if there is one, else a lazily-adopted main-stack fiber (adopted while
// provably standing on it — current()==0 means exactly that). The stamp is
// consumed synchronously: a coroutine entered by this jump yields back
// before any other stack can jump out of the root, so re-stamping per jump
// is race-free on a single thread.
pcbjam_sched::ContextId resolve_root_identity()
{
    const pcbjam_sched::ContextId cur = pcbjam_sched::current();

    if( cur )
        return cur;

    if( !g_main_stack_sched_id )
        g_main_stack_sched_id =
                pcbjam_sched::fiber_adopt_current( ASYNCIFY_STACK_SIZE, "libctx-main" );

    return g_main_stack_sched_id;
}


void ensure_main_context()
{
    if( !g_main_initialized )
    {
        g_main_context.initialized = true;
        g_main_context.releasable = false;
        g_main_context.id = g_next_context_id++;

        g_main_context.sched_id = resolve_root_identity();
        ++g_main_refresh_count;
        g_main_initialized = true;
        g_main_context.running = true;
        g_main_context.swap_suspended = false;
        g_current_context = &g_main_context;
        log_wasm_fcontext( "main-refresh", &g_main_context, &g_main_context, 0 );
        return;
    }

    if( !g_current_context )
        g_current_context = &g_main_context;

    g_main_context.running = true;
}


// QEMU-style trampoline: the entry function NEVER returns.
// Per Emscripten fiber.h: "If entry_func returns, the entire program will end."
// The while(true) loop ensures we stay inside the fiber's stack frame forever.
// After the coroutine body finishes, we swap back to the caller using the
// coroutine's own heap-allocated fiber (not a stack-local temporary).
[[noreturn]] void wasm_fcontext_entry( void* aArg )
{
    auto* ctx = static_cast<wasm_fcontext*>( aArg );

    while( true )
    {
        log_wasm_fcontext( "entry-call", ctx, ctx->return_to, ctx->transfer_value );
        ctx->entry( ctx->transfer_value );

        ctx->finished = true;
        ctx->running = false;
        log_wasm_fcontext( "entry-returned", ctx, ctx->return_to, 0 );

        if( !ctx->return_to )
        {
            log_wasm_fcontext( "entry-orphaned", ctx, nullptr, 0 );
            EM_ASM( { console.log( "[collab-fcontext] entry-orphaned ctx=" + $0 ); }, (int) ctx->id );
            std::abort();
        }

        // Swap back to whoever started us, through the scheduler.
        wasm_fcontext* return_to = ctx->return_to;
        return_to->transfer_value = 0;
        g_current_context = return_to;
        return_to->running = true;
        // Mirror of jump_fcontext's bookkeeping (see wasm_fcontext).
        ctx->swap_suspended = true;
        return_to->swap_suspended = false;

        log_wasm_fcontext( "trampoline-swap", ctx, return_to, 0 );

        // Phase B, transfer lane: a finished coroutine's hand-back is
        // TERMINAL. The legacy while(true) ghost re-entry loop, expressed as
        // transfers, marked the counterpart Ready on every bounce and
        // livelocked two finished coroutines across ticks (measured
        // 2026-08-07, D-on probe). fiber_finish_transfer marks this context
        // Finished — never re-queued; a later transfer into it is refused
        // into the caller's ghost contract — and never returns.
        if( pcbjam_sched::current() == ctx->sched_id && pcbjam_sched::can_yield_here() )
        {
            ctx->swap_suspended = false;   // terminal: no valid re-entry capture
            pcbjam_sched::fiber_finish_transfer( ctx->sched_id, return_to->sched_id, 0 );
            // Not reached: the registry parks this fiber forever.
        }

        // Direct-swap fallback (nothing on contexts in this build): legacy
        // semantics unchanged, including ghost re-entry below.
        ctx->transfer_value = sched_swap( ctx, return_to, 0 );

        // If someone swaps back to us, the while(true) loops.
        // Reset state so re-entry is safe (though unlikely in normal operation).
        log_wasm_fcontext( "trampoline-reenter", ctx, ctx->return_to, ctx->transfer_value );
        ctx->finished = false;
        ctx->running = true;
    }
}

} // namespace


fcontext_t LIBCONTEXT_CALL_CONVENTION make_fcontext( void* sp, size_t size,
        void (* fn)( intptr_t ) )
{
    // A new coroutine is the natural moment to reclaim: whatever the ring
    // still holds has survived at least one full tool cycle.
    trim_grace_ring();

    auto* ctx = new( std::nothrow ) wasm_fcontext();

    if( !ctx )
        return nullptr;

    ctx->entry = fn;
    ctx->initialized = true;
    ctx->refcount = 1;
    ctx->id = g_next_context_id++;
    // A fresh fiber is safely enterable: its first swap-in takes the
    // entry-point path (no rewind of saved data is involved). The registry
    // records the same fact as Status::Fresh.
    ctx->swap_suspended = true;

    void* stack_bottom = static_cast<char*>( sp ) - size;

    ctx->sched_id = pcbjam_sched::fiber_create( wasm_fcontext_entry, ctx,
                                                stack_bottom, size,
                                                ASYNCIFY_STACK_SIZE, "tool-fiber" );

    if( !ctx->sched_id )
    {
        delete ctx;
        return nullptr;
    }

    return ctx;
}


intptr_t LIBCONTEXT_CALL_CONVENTION jump_fcontext( fcontext_t* ofc, fcontext_t nfc,
        intptr_t vp, bool preserve_fpu )
{
    (void) preserve_fpu;

    ensure_main_context();

    auto* old_ctx = g_current_context;
    auto* new_ctx = static_cast<wasm_fcontext*>( nfc );

    if( !old_ctx || !new_ctx || !new_ctx->initialized )
        return 0;

    // A jump OUT of the root re-resolves which stack the root IS right now
    // (the running scheduler context, or the lazily-adopted main stack — see
    // resolve_root_identity). The stamp is read again by the coroutine's
    // yield-back, which happens synchronously before any other stack can
    // jump out of the root.
    if( old_ctx == &g_main_context )
        old_ctx->sched_id = resolve_root_identity();

    if( !new_ctx->sched_id )
    {
        // Its fiber was reclaimed from the grace ring: there is no stack to
        // enter. Drop the dispatch through the established ghost contract.
        beacon_jump_into_reclaimed( new_ctx );
        return 0;
    }

    // Phase A moves the BOOKKEEPING, not the decision. `swap_suspended` stays
    // authoritative here and the registry only observes, because Phase A has
    // no authority to refuse a dispatch the legacy backend permitted: keying
    // the refusal on `fiber_enterable()` regressed exactly that way (measured
    // 2026-08-06, quasimodal-strand — Symbol Properties never opened, because
    // a fiber the registry still called Running was refused while
    // `swap_suspended` said it was validly suspended). The decision moves at
    // Phase B, when the star owns coroutine lifetimes and the registry's view
    // is the only view there is.
    //
    // Until then every disagreement is a recorded fact, not a behaviour
    // change: these beacons are the evidence Phase B's flip gets designed on.
    // Who does the registry think is on the CPU? Under the star (Phase B) the
    // authority is current() — the scheduler enters every context, and the
    // symmetric lane's fiber_current() is only meaningful while a direct swap
    // chain owns the CPU. Comparing against the lane alone made this beacon
    // fire on every transfer jump (28 firings in one battery, all benign),
    // which is exactly how a tripwire stops being evidence.
    const pcbjam_sched::ContextId on_cpu = pcbjam_sched::current()
                                                   ? pcbjam_sched::current()
                                                   : pcbjam_sched::fiber_current();

    // "The registry says nobody is running" and "we are the root, on the main
    // stack" are the SAME fact, not a disagreement: entries that still arrive
    // on the main stack (mailbox timers, DOM handlers, everything until Phase
    // E) run there with no context on the CPU.
    const bool root_on_main_stack = old_ctx == &g_main_context && on_cpu == 0
                                    && old_ctx->sched_id == g_main_stack_sched_id;

    if( !root_on_main_stack && old_ctx->sched_id != on_cpu )
        divergence_beacon( "current", old_ctx, new_ctx );

    if( pcbjam_sched::fiber_enterable( new_ctx->sched_id ) != new_ctx->swap_suspended )
        divergence_beacon( "enterable", old_ctx, new_ctx );

    if( !new_ctx->swap_suspended )
    {
        // The target is mid-execution: its body is asyncify-parked inside
        // handleSleep somewhere below a JS turn (TOOL_MANAGER cannot tell —
        // natively a coroutine cannot be suspended without yielding). Its
        // saved fiber suspension is STALE; swapping in would rewind it
        // regardless (finishContextSwitch → doRewind → "unreachable
        // executed", the production board-load trap) and poison all later
        // Asyncify entries. REFUSE instead — the same null-INVOCATION_ARGS
        // contract the jump-ghost path below already established. The parked
        // body completes via its own wake and suspends properly; only this
        // one dispatch is dropped.
        log_wasm_fcontext( "jump-refused-parked", new_ctx, old_ctx, vp );

        static int s_refused = 0;
        ++s_refused;

        if( s_refused <= 10 || s_refused % 100 == 0 )
        {
            EM_ASM( { console.warn( "[collab-fcontext] jump-refused: ctx=" + $0
                                    + " is asyncify-parked, not suspended (occurrence "
                                    + $1 + ")" ); },
                    (int) new_ctx->id, s_refused );
        }

        return 0;
    }

    // REFUSAL RETRACTED 2026-08-03 (round 6): refusing this swap DOES prevent
    // the unrewindable write (the "unreachable executed" cascade disappears),
    // but the dropped dispatch strands the tool coroutine and the load dies
    // anyway — the frame tears down, ~wxTopLevelWindowWasm fires the host quit
    // notification, and the teardown itself traps "index out of bounds". Same
    // dead board, different obituary. Kept as a BEACON so field logs still name
    // the exact fatal interleave; the cure has to stop the situation from
    // arising (fiber-first runtime / requeueing the dispatch to a fresh JS
    // entry), not veto it after the fact.
    if( false && old_ctx == &g_main_context && new_ctx != &g_main_context
            && wasm_root_wake_in_flight() )
    {
        // Swapping OUT of main into a fiber inside main's own live wake window
        // would write an unrewindable MAIN suspension (see
        // wasm_root_wake_in_flight above) — the 2026-07/08 production
        // board-load killer, reproduced locally 2026-08-03 (warm load of a
        // 110MB+ project stretches one wake slice long enough for an event
        // dispatch to land inside it). REFUSE with the same
        // null-INVOCATION_ARGS ghost contract as the parked guard above: this
        // one dispatch is dropped, the target stays validly suspended, and the
        // next fresh-entry tick redelivers.
        //
        // Jumps INTO main are always allowed — they consume main's EXISTING
        // suspension (a fiber's yield-back, possibly with g_current_context
        // laundered to null→main); refusing one mid-yield strands the fiber's
        // completion and took the whole frame down (guard round 1, 2026-08-03:
        // the refused into-main jump cascaded into a real top-window close).
        log_wasm_fcontext( "jump-refused-hot-main", new_ctx, old_ctx, vp );

        static int s_hot_refused = 0;
        ++s_hot_refused;

        if( s_hot_refused <= 10 || s_hot_refused % 100 == 0 )
        {
            // The stack names the JS entry that drove this dispatch (timer,
            // RAF, message) plus the wasm frame chain — the only way to
            // identify the refused dispatch's owner in a release build.
            EM_ASM( { console.warn( "[collab-fcontext] jump-refused-hot-main: old=" + $0
                                    + " new=" + $1
                                    + " dispatch inside main's live wake window (occurrence "
                                    + $2 + ")\n" + new Error( "refusal-site" ).stack ); },
                    (int) old_ctx->id, (int) new_ctx->id, s_hot_refused );
        }

        return 0;
    }

    // THE fatal interleave, observed not vetoed: main swapping OUT into a fiber
    // inside its own live sleep-wake continuation. emscripten_fiber_swap stamps
    // exportCallStack[0] (the wake's re-invoked __main_argc_argv) as the rewind
    // entry of a capture that only spans the swap-site frames — an unrewindable
    // pairing. Exactly one of these precedes every reproduced board-load death.
    if( old_ctx == &g_main_context && new_ctx != &g_main_context
            && wasm_root_wake_in_flight() )
    {
        static int s_hot_out = 0;
        ++s_hot_out;

        if( s_hot_out <= 10 || s_hot_out % 100 == 0 )
        {
            EM_ASM( { console.warn( "[collab-fcontext] hot-main-swap-out: old=" + $0
                                    + " new=" + $1
                                    + " — unrewindable main suspension written (occurrence "
                                    + $2 + ")" ); },
                    (int) old_ctx->id, (int) new_ctx->id, s_hot_out );
        }
    }

    // A hot jump INTO main is either a fiber's legit yield-back or the return
    // half of the pair above; beaconed for field correlation.
    if( new_ctx == &g_main_context && wasm_root_wake_in_flight() )
    {
        static int s_hot_into_main = 0;
        ++s_hot_into_main;

        if( s_hot_into_main <= 10 || s_hot_into_main % 100 == 0 )
        {
            EM_ASM( { console.warn( "[collab-fcontext] jump-hot-into-main: old=" + $0
                                    + " new=" + $1 + " (occurrence " + $2 + ")" ); },
                    (int) old_ctx->id, (int) new_ctx->id, s_hot_into_main );
        }
    }

    log_wasm_fcontext( "jump-enter", new_ctx, old_ctx, vp, ofc,
                       ofc ? static_cast<wasm_fcontext*>( *ofc ) : nullptr );
    assign_saved_context( ofc, old_ctx );

    if( new_ctx->entry && !new_ctx->return_to )
        new_ctx->return_to = old_ctx;

    const uint32_t expected_resume_epoch = old_ctx->resume_epoch;
    ++new_ctx->resume_epoch;
    new_ctx->transfer_value = vp;
    old_ctx->running = false;
    new_ctx->running = true;
    // The swap's unwind writes old_ctx's fiber suspension (valid to resume);
    // new_ctx is about to execute (its saved data is consumed by this swap).
    old_ctx->swap_suspended = true;
    new_ctx->swap_suspended = false;
    g_current_context = new_ctx;

    log_wasm_fcontext( "jump-swap", new_ctx, old_ctx, vp );
    old_ctx->transfer_value = sched_swap( old_ctx, new_ctx, vp );

    if( old_ctx->resume_epoch == expected_resume_epoch )
    {
        // Ghost resume: nobody officially swapped back to us (epoch unchanged).
        // Instead of killing all WASM execution, log and return 0 so the caller
        // sees a null INVOCATION_ARGS and handles it gracefully.
        log_wasm_fcontext( "jump-ghost", old_ctx, new_ctx, old_ctx->transfer_value );
        // Unconditional anomaly beacon (drift-trio #10b hunt): ghost resumes
        // precede the rewind-path table-index traps under collab fuzz load.
        EM_ASM( { console.log( "[collab-fcontext] jump-ghost ctx=" + $0 + " epoch=" + $1 ); },
                (int) old_ctx->id, (int) old_ctx->resume_epoch );
        g_current_context = old_ctx;
        old_ctx->running = true;
        return 0;
    }

    g_current_context = old_ctx;
    old_ctx->running = true;
    log_wasm_fcontext( "jump-resume", old_ctx, new_ctx, old_ctx->transfer_value );
    return old_ctx->transfer_value;
}


void LIBCONTEXT_CALL_CONVENTION release_fcontext( fcontext_t ctx )
{
    release_context( static_cast<wasm_fcontext*>( ctx ) );
}

} // namespace libcontext

#endif // LIBCONTEXT_PLATFORM_wasm32

#if defined(LIBCONTEXT_PLATFORM_windows_i386) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".p2align 4,,15\n"
".globl	_jump_fcontext\n"
".def	_jump_fcontext;	.scl	2;	.type	32;	.endef\n"
"_jump_fcontext:\n"
"    mov    0x10(%esp),%ecx\n"
"    push   %ebp\n"
"    push   %ebx\n"
"    push   %esi\n"
"    push   %edi\n"
"    mov    %fs:0x18,%edx\n"
"    mov    (%edx),%eax\n"
"    push   %eax\n"
"    mov    0x4(%edx),%eax\n"
"    push   %eax\n"
"    mov    0x8(%edx),%eax\n"
"    push   %eax\n"
"    mov    0xe0c(%edx),%eax\n"
"    push   %eax\n"
"    mov    0x10(%edx),%eax\n"
"    push   %eax\n"
"    lea    -0x8(%esp),%esp\n"
"    test   %ecx,%ecx\n"
"    je     nxt1\n"
"    stmxcsr (%esp)\n"
"    fnstcw 0x4(%esp)\n"
"nxt1:\n"
"    mov    0x30(%esp),%eax\n"
"    mov    %esp,(%eax)\n"
"    mov    0x34(%esp),%edx\n"
"    mov    0x38(%esp),%eax\n"
"    mov    %edx,%esp\n"
"    test   %ecx,%ecx\n"
"    je     nxt2\n"
"    ldmxcsr (%esp)\n"
"    fldcw  0x4(%esp)\n"
"nxt2:\n"
"    lea    0x8(%esp),%esp\n"
"    mov    %fs:0x18,%edx\n"
"    pop    %ecx\n"
"    mov    %ecx,0x10(%edx)\n"
"    pop    %ecx\n"
"    mov    %ecx,0xe0c(%edx)\n"
"    pop    %ecx\n"
"    mov    %ecx,0x8(%edx)\n"
"    pop    %ecx\n"
"    mov    %ecx,0x4(%edx)\n"
"    pop    %ecx\n"
"    mov    %ecx,(%edx)\n"
"    pop    %edi\n"
"    pop    %esi\n"
"    pop    %ebx\n"
"    pop    %ebp\n"
"    pop    %edx\n"
"    mov    %eax,0x4(%esp)\n"
"    jmp    *%edx\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_windows_i386) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".p2align 4,,15\n"
".globl	_make_fcontext\n"
".def	_make_fcontext;	.scl	2;	.type	32;	.endef\n"
"_make_fcontext:\n"
"mov    0x4(%esp),%eax\n"
"lea    -0x8(%eax),%eax\n"
"and    $0xfffffff0,%eax\n"
"lea    -0x3c(%eax),%eax\n"
"mov    0x4(%esp),%ecx\n"
"mov    %ecx,0x14(%eax)\n"
"mov    0x8(%esp),%edx\n"
"neg    %edx\n"
"lea    (%ecx,%edx,1),%ecx\n"
"mov    %ecx,0x10(%eax)\n"
"mov    %ecx,0xc(%eax)\n"
"mov    0xc(%esp),%ecx\n"
"mov    %ecx,0x2c(%eax)\n"
"stmxcsr (%eax)\n"
"fnstcw 0x4(%eax)\n"
"mov    $finish,%ecx\n"
"mov    %ecx,0x30(%eax)\n"
"mov    %fs:0x0,%ecx\n"
"walk:\n"
"mov    (%ecx),%edx\n"
"inc    %edx\n"
"je     found\n"
"dec    %edx\n"
"xchg   %edx,%ecx\n"
"jmp    walk\n"
"found:\n"
"mov    0x4(%ecx),%ecx\n"
"mov    %ecx,0x3c(%eax)\n"
"mov    $0xffffffff,%ecx\n"
"mov    %ecx,0x38(%eax)\n"
"lea    0x38(%eax),%ecx\n"
"mov    %ecx,0x18(%eax)\n"
"ret\n"
"finish:\n"
"xor    %eax,%eax\n"
"mov    %eax,(%esp)\n"
"call   _exit\n"
"hlt\n"
".def	__exit;	.scl	2;	.type	32;	.endef  \n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_windows_x86_64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".p2align 4,,15\n"
".globl	jump_fcontext\n"
".def	jump_fcontext;	.scl	2;	.type	32;	.endef\n"
".seh_proc	jump_fcontext\n"
"jump_fcontext:\n"
".seh_endprologue\n"
"	push   %rbp\n"
"	push   %rbx\n"
"	push   %rsi\n"
"	push   %rdi\n"
"	push   %r15\n"
"	push   %r14\n"
"	push   %r13\n"
"	push   %r12\n"
"	mov    %gs:0x30,%r10\n"
"	mov    0x8(%r10),%rax\n"
"	push   %rax\n"
"	mov    0x10(%r10),%rax\n"
"	push   %rax\n"
"	mov    0x1478(%r10),%rax\n"
"	push   %rax\n"
"	mov    0x20(%r10),%rax\n"
"	push   %rax\n"
"	lea    -0xa8(%rsp),%rsp\n"
"	test   %r9,%r9\n"
"	je     nxt1\n"
"	stmxcsr 0xa0(%rsp)\n"
"	fnstcw 0xa4(%rsp)\n"
"	movaps %xmm6,(%rsp)\n"
"	movaps %xmm7,0x10(%rsp)\n"
"	movaps %xmm8,0x20(%rsp)\n"
"	movaps %xmm9,0x30(%rsp)\n"
"	movaps %xmm10,0x40(%rsp)\n"
"	movaps %xmm11,0x50(%rsp)\n"
"	movaps %xmm12,0x60(%rsp)\n"
"	movaps %xmm13,0x70(%rsp)\n"
"	movaps %xmm14,0x80(%rsp)\n"
"	movaps %xmm15,0x90(%rsp)\n"
"nxt1:\n"
"	xor    %r10,%r10\n"
"	push   %r10\n"
"	mov    %rsp,(%rcx)\n"
"	mov    %rdx,%rsp\n"
"	pop    %r10\n"
"	test   %r9,%r9\n"
"	je     nxt2\n"
"	ldmxcsr 0xa0(%rsp)\n"
"	fldcw  0xa4(%rsp)\n"
"	movaps (%rsp),%xmm6\n"
"	movaps 0x10(%rsp),%xmm7\n"
"	movaps 0x20(%rsp),%xmm8\n"
"	movaps 0x30(%rsp),%xmm9\n"
"	movaps 0x40(%rsp),%xmm10\n"
"	movaps 0x50(%rsp),%xmm11\n"
"	movaps 0x60(%rsp),%xmm12\n"
"	movaps 0x70(%rsp),%xmm13\n"
"	movaps 0x80(%rsp),%xmm14\n"
"	movaps 0x90(%rsp),%xmm15\n"
"nxt2:\n"
"	mov    $0xa8,%rcx\n"
"    test   %r10,%r10\n"
"    je     nxt3\n"
"    add    $0x8,%rcx\n"
"nxt3:\n"
"	lea    (%rsp,%rcx,1),%rsp\n"
"	mov    %gs:0x30,%r10\n"
"	pop    %rax\n"
"	mov    %rax,0x20(%r10)\n"
"	pop    %rax\n"
"	mov    %rax,0x1478(%r10)\n"
"	pop    %rax\n"
"	mov    %rax,0x10(%r10)\n"
"	pop    %rax\n"
"	mov    %rax,0x8(%r10)\n"
"	pop    %r12\n"
"	pop    %r13\n"
"	pop    %r14\n"
"	pop    %r15\n"
"	pop    %rdi\n"
"	pop    %rsi\n"
"	pop    %rbx\n"
"	pop    %rbp\n"
"	pop    %r10\n"
"	mov    %r8,%rax\n"
"	mov    %r8,%rcx\n"
"	jmpq   *%r10\n"
".seh_endproc\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_windows_x86_64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".p2align 4,,15\n"
".globl	make_fcontext\n"
".def	make_fcontext;	.scl	2;	.type	32;	.endef\n"
".seh_proc	make_fcontext\n"
"make_fcontext:\n"
".seh_endprologue\n"
"mov    %rcx,%rax\n"
"sub    $0x28,%rax\n"
"and    $0xfffffffffffffff0,%rax\n"
"sub    $0x128,%rax\n"
"mov    %r8,0x118(%rax)\n"
"mov    %rcx,0xd0(%rax)\n"
"neg    %rdx\n"
"lea    (%rcx,%rdx,1),%rcx\n"
"mov    %rcx,0xc8(%rax)\n"
"mov    %rcx,0xc0(%rax)\n"
"stmxcsr 0xa8(%rax)\n"
"fnstcw 0xac(%rax)\n"
"leaq  finish(%rip), %rcx\n"
"mov    %rcx,0x120(%rax)\n"
"mov    $0x1,%rcx\n"
"mov    %rcx,(%rax)\n"
"retq\n"
"finish:\n"
"xor    %rcx,%rcx\n"
"call   _exit\n"
"hlt\n"
"   .seh_endproc\n"
".def	_exit;	.scl	2;	.type	32;	.endef  \n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_i386) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl jump_fcontext\n"
".align 2\n"
".type jump_fcontext,@function\n"
"jump_fcontext:\n"
"    movl  0x10(%esp), %ecx\n"
"    pushl  %ebp  \n"
"    pushl  %ebx  \n"
"    pushl  %esi  \n"
"    pushl  %edi  \n"
"    leal  -0x8(%esp), %esp\n"
"    test  %ecx, %ecx\n"
"    je  1f\n"
"    stmxcsr  (%esp)\n"
"    fnstcw  0x4(%esp)\n"
"1:\n"
"    movl  0x1c(%esp), %eax\n"
"    movl  %esp, (%eax)\n"
"    movl  0x20(%esp), %edx\n"
"    movl  0x24(%esp), %eax\n"
"    movl  %edx, %esp\n"
"    test  %ecx, %ecx\n"
"    je  2f\n"
"    ldmxcsr  (%esp)\n"
"    fldcw  0x4(%esp)\n"
"2:\n"
"    leal  0x8(%esp), %esp\n"
"    popl  %edi  \n"
"    popl  %esi  \n"
"    popl  %ebx  \n"
"    popl  %ebp  \n"
"    popl  %edx\n"
"    movl  %eax, 0x4(%esp)\n"
"    jmp  *%edx\n"
".size jump_fcontext,.-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_i386) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl make_fcontext\n"
".align 2\n"
".type make_fcontext,@function\n"
"make_fcontext:\n"
"    movl  0x4(%esp), %eax\n"
"    leal  -0x8(%eax), %eax\n"
"    andl  $-16, %eax\n"
"    leal  -0x20(%eax), %eax\n"
"    movl  0xc(%esp), %edx\n"
"    movl  %edx, 0x18(%eax)\n"
"    stmxcsr  (%eax)\n"
"    fnstcw  0x4(%eax)\n"
"    call  1f\n"
"1:  popl  %ecx\n"
"    addl  $finish-1b, %ecx\n"
"    movl  %ecx, 0x1c(%eax)\n"
"    ret \n"
"finish:\n"
"    call  2f\n"
"2:  popl  %ebx\n"
"    addl  $_GLOBAL_OFFSET_TABLE_+[.-2b], %ebx\n"
"    xorl  %eax, %eax\n"
"    movl  %eax, (%esp)\n"
"    call  _exit@PLT\n"
"    hlt\n"
".size make_fcontext,.-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_x86_64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl jump_fcontext\n"
".type jump_fcontext,@function\n"
".align 16\n"
"jump_fcontext:\n"
"    pushq  %rbp  \n"
"    pushq  %rbx  \n"
"    pushq  %r15  \n"
"    pushq  %r14  \n"
"    pushq  %r13  \n"
"    pushq  %r12  \n"
"    leaq  -0x8(%rsp), %rsp\n"
"    cmp  $0, %rcx\n"
"    je  1f\n"
"    stmxcsr  (%rsp)\n"
"    fnstcw   0x4(%rsp)\n"
"1:\n"
"    movq  %rsp, (%rdi)\n"
"    movq  %rsi, %rsp\n"
"    cmp  $0, %rcx\n"
"    je  2f\n"
"    ldmxcsr  (%rsp)\n"
"    fldcw  0x4(%rsp)\n"
"2:\n"
"    leaq  0x8(%rsp), %rsp\n"
"    popq  %r12  \n"
"    popq  %r13  \n"
"    popq  %r14  \n"
"    popq  %r15  \n"
"    popq  %rbx  \n"
"    popq  %rbp  \n"
"    popq  %r8\n"
"    movq  %rdx, %rax\n"
"    movq  %rdx, %rdi\n"
"    jmp  *%r8\n"
".size jump_fcontext,.-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_x86_64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl make_fcontext\n"
".type make_fcontext,@function\n"
".align 16\n"
"make_fcontext:\n"
"    movq  %rdi, %rax\n"
"    andq  $-16, %rax\n"
"    leaq  -0x48(%rax), %rax\n"
"    movq  %rdx, 0x38(%rax)\n"
"    stmxcsr  (%rax)\n"
"    fnstcw   0x4(%rax)\n"
"    leaq  finish(%rip), %rcx\n"
"    movq  %rcx, 0x40(%rax)\n"
"    ret \n"
"finish:\n"
"    xorq  %rdi, %rdi\n"
"    call  _exit@PLT\n"
"    hlt\n"
".size make_fcontext,.-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_apple_x86_64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl _jump_fcontext\n"
".align 8\n"
"_jump_fcontext:\n"
"    pushq  %rbp  \n"
"    pushq  %rbx  \n"
"    pushq  %r15  \n"
"    pushq  %r14  \n"
"    pushq  %r13  \n"
"    pushq  %r12  \n"
"    leaq  -0x8(%rsp), %rsp\n"
"    cmp  $0, %rcx\n"
"    je  1f\n"
"    stmxcsr  (%rsp)\n"
"    fnstcw   0x4(%rsp)\n"
"1:\n"
"    movq  %rsp, (%rdi)\n"
"    movq  %rsi, %rsp\n"
"    cmp  $0, %rcx\n"
"    je  2f\n"
"    ldmxcsr  (%rsp)\n"
"    fldcw  0x4(%rsp)\n"
"2:\n"
"    leaq  0x8(%rsp), %rsp\n"
"    popq  %r12  \n"
"    popq  %r13  \n"
"    popq  %r14  \n"
"    popq  %r15  \n"
"    popq  %rbx  \n"
"    popq  %rbp  \n"
"    popq  %r8\n"
"    movq  %rdx, %rax\n"
"    movq  %rdx, %rdi\n"
"    jmp  *%r8\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_apple_x86_64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl _make_fcontext\n"
".align 8\n"
"_make_fcontext:\n"
"    movq  %rdi, %rax\n"
"    movabs  $-16,           %r8\n"
"    andq    %r8,            %rax\n"
"    leaq  -0x48(%rax), %rax\n"
"    movq  %rdx, 0x38(%rax)\n"
"    stmxcsr  (%rax)\n"
"    fnstcw   0x4(%rax)\n"
"    leaq  finish(%rip), %rcx\n"
"    movq  %rcx, 0x40(%rax)\n"
"    ret \n"
"finish:\n"
"    xorq  %rdi, %rdi\n"
"    call  __exit\n"
"    hlt\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_apple_i386) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl _jump_fcontext\n"
".align 2\n"
"_jump_fcontext:\n"
"    movl  0x10(%esp), %ecx\n"
"    pushl  %ebp  \n"
"    pushl  %ebx  \n"
"    pushl  %esi  \n"
"    pushl  %edi  \n"
"    leal  -0x8(%esp), %esp\n"
"    test  %ecx, %ecx\n"
"    je  1f\n"
"    stmxcsr  (%esp)\n"
"    fnstcw  0x4(%esp)\n"
"1:\n"
"    movl  0x1c(%esp), %eax\n"
"    movl  %esp, (%eax)\n"
"    movl  0x20(%esp), %edx\n"
"    movl  0x24(%esp), %eax\n"
"    movl  %edx, %esp\n"
"    test  %ecx, %ecx\n"
"    je  2f\n"
"    ldmxcsr  (%esp)\n"
"    fldcw  0x4(%esp)\n"
"2:\n"
"    leal  0x8(%esp), %esp\n"
"    popl  %edi  \n"
"    popl  %esi  \n"
"    popl  %ebx  \n"
"    popl  %ebp  \n"
"    popl  %edx\n"
"    movl  %eax, 0x4(%esp)\n"
"    jmp  *%edx\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_apple_i386) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl _make_fcontext\n"
".align 2\n"
"_make_fcontext:\n"
"    movl  0x4(%esp), %eax\n"
"    leal  -0x8(%eax), %eax\n"
"    andl  $-16, %eax\n"
"    leal  -0x20(%eax), %eax\n"
"    movl  0xc(%esp), %edx\n"
"    movl  %edx, 0x18(%eax)\n"
"    stmxcsr  (%eax)\n"
"    fnstcw  0x4(%eax)\n"
"    call  1f\n"
"1:  popl  %ecx\n"
"    addl  $finish-1b, %ecx\n"
"    movl  %ecx, 0x1c(%eax)\n"
"    ret \n"
"finish:\n"
"    xorl  %eax, %eax\n"
"    movl  %eax, (%esp)\n"
"    call  __exit\n"
"    hlt\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_apple_arm64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".balign  16\n"
".global _jump_fcontext\n"
"_jump_fcontext:\n"
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
"    # bti c\n"
"    hint #34\n"
#endif
"    # prepare stack for GP + FPU\n"
"    sub  sp, sp, #0xb0\n"
"    # test if fpu env should be preserved\n"
"    cmp  w3, #0\n"
"    b.eq  1f\n"
"    # save d8 - d15\n"
"    stp  d8,  d9,  [sp, #0x00]\n"
"    stp  d10, d11, [sp, #0x10]\n"
"    stp  d12, d13, [sp, #0x20]\n"
"    stp  d14, d15, [sp, #0x30]\n"
"1:\n"
"    # save x19-x30\n"
"    stp  x19, x20, [sp, #0x40]\n"
"    stp  x21, x22, [sp, #0x50]\n"
"    stp  x23, x24, [sp, #0x60]\n"
"    stp  x25, x26, [sp, #0x70]\n"
"    stp  x27, x28, [sp, #0x80]\n"
"    stp  x29, x30, [sp, #0x90]\n"
"    # save LR as PC\n"
"    str  x30, [sp, #0xa0]\n"
"    # store RSP (pointing to context-data) in first argument (x0).\n"
"    # STR cannot have sp as a target register\n"
"    mov  x4, sp\n"
"    str  x4, [x0]\n"
"    # restore RSP (pointing to context-data) from A2 (x1)\n"
"    mov  sp, x1\n"
"    # test if fpu env should be preserved\n"
"    cmp  w3, #0\n"
"    b.eq  2f\n"
"    # load d8 - d15\n"
"    ldp  d8,  d9,  [sp, #0x00]\n"
"    ldp  d10, d11, [sp, #0x10]\n"
"    ldp  d12, d13, [sp, #0x20]\n"
"    ldp  d14, d15, [sp, #0x30]\n"
"2:\n"
"    # load x19-x30\n"
"    ldp  x19, x20, [sp, #0x40]\n"
"    ldp  x21, x22, [sp, #0x50]\n"
"    ldp  x23, x24, [sp, #0x60]\n"
"    ldp  x25, x26, [sp, #0x70]\n"
"    ldp  x27, x28, [sp, #0x80]\n"
"    ldp  x29, x30, [sp, #0x90]\n"
"    # use third arg as return value after jump\n"
"    # and as first arg in context function\n"
"    mov  x0, x2\n"
"    # load pc\n"
"    ldr  x4, [sp, #0xa0]\n"
"    # restore stack from GP + FPU\n"
"    add  sp, sp, #0xb0\n"
"    ret x4\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_apple_arm64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".cpu    generic+fp+simd\n"
".text\n"
".align  2\n"
".global _make_fcontext\n"
"_make_fcontext:\n"
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
"    # bti c\n"
"    hint #34\n"
#endif
"    # shift address in x0 (allocated stack) to lower 16 byte boundary\n"
"    and x0, x0, ~0xF\n"
"    # reserve space for context-data on context-stack\n"
"    sub  x0, x0, #0xb0\n"
"    # third arg of make_fcontext() == address of context-function\n"
"    # store address as a PC to jump in\n"
"    str  x2, [x0, #0xa0]\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns (LR register)\n"
"    # need to relocate manually because of Clang limitation\n"
"    adrp x1, finish@PAGE\n"
"    add  x1, x1, finish@PAGEOFF\n"
"    str  x1, [x0, #0x98]\n"
"    ret  x30 \n"
"finish:\n"
"    # exit code is zero\n"
"    mov  x0, #0\n"
"    # exit application\n"
"    bl  _exit\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_arm32) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl jump_fcontext\n"
".align 2\n"
".type jump_fcontext,%function\n"
"jump_fcontext:\n"
"    @ save LR as PC\n"
"    push {lr}\n"
"    @ save V1-V8,LR\n"
"    push {v1-v8,lr}\n"
"    @ prepare stack for FPU\n"
"    sub  sp, sp, #64\n"
"    @ test if fpu env should be preserved\n"
"    cmp  a4, #0\n"
"    beq  1f\n"
"    @ save S16-S31\n"
"    vstmia  sp, {d8-d15}\n"
"1:\n"
"    @ store RSP (pointing to context-data) in A1\n"
"    str  sp, [a1]\n"
"    @ restore RSP (pointing to context-data) from A2\n"
"    mov  sp, a2\n"
"    @ test if fpu env should be preserved\n"
"    cmp  a4, #0\n"
"    beq  2f\n"
"    @ restore S16-S31\n"
"    vldmia  sp, {d8-d15}\n"
"2:\n"
"    @ prepare stack for FPU\n"
"    add  sp, sp, #64\n"
"    @ use third arg as return value after jump\n"
"    @ and as first arg in context function\n"
"    mov  a1, a3\n"
"    @ restore v1-V8,LR,PC\n"
"    pop {v1-v8,lr}\n"
"    pop {pc}\n"
".size jump_fcontext,.-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_arm32) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl make_fcontext\n"
".align 2\n"
".type make_fcontext,%function\n"
"make_fcontext:\n"
"    @ shift address in A1 to lower 16 byte boundary\n"
"    bic  a1, a1, #15\n"
"    @ reserve space for context-data on context-stack\n"
"    sub  a1, a1, #104\n"
"    @ third arg of make_fcontext() == address of context-function\n"
"    str  a3, [a1,#100]\n"
"    @ compute abs address of label finish\n"
"    adr  a2, finish\n"
"    @ save address of finish as return-address for context-function\n"
"    @ will be entered after context-function returns\n"
"    str  a2, [a1,#96]\n"
"    bx  lr @ return pointer to context-data\n"
"finish:\n"
"    @ exit code is zero\n"
"    mov  a1, #0\n"
"    @ exit application\n"
"    bl  _exit@PLT\n"
".size make_fcontext,.-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_arm64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".cpu    generic+fp+simd\n"
".text\n"
".align  2\n"
".global jump_fcontext\n"
".type   jump_fcontext, %function\n"
"jump_fcontext:\n"
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
"    # bti c\n"
"    hint #34\n"
#endif
"    # prepare stack for GP + FPU\n"
"    sub  sp, sp, #0xb0\n"
"# Because gcc may save integer registers in fp registers across a\n"
"# function call we cannot skip saving the fp registers.\n"
"#\n"
"# Do not reinstate this test unless you fully understand what you\n"
"# are doing.\n"
"#\n"
"#    # test if fpu env should be preserved\n"
"#    cmp  w3, #0\n"
"#    b.eq  1f\n"
"    # save d8 - d15\n"
"    stp  d8,  d9,  [sp, #0x00]\n"
"    stp  d10, d11, [sp, #0x10]\n"
"    stp  d12, d13, [sp, #0x20]\n"
"    stp  d14, d15, [sp, #0x30]\n"
"1:\n"
"    # save x19-x30\n"
"    stp  x19, x20, [sp, #0x40]\n"
"    stp  x21, x22, [sp, #0x50]\n"
"    stp  x23, x24, [sp, #0x60]\n"
"    stp  x25, x26, [sp, #0x70]\n"
"    stp  x27, x28, [sp, #0x80]\n"
"    stp  x29, x30, [sp, #0x90]\n"
"    # save LR as PC\n"
"    str  x30, [sp, #0xa0]\n"
"    # store RSP (pointing to context-data) in first argument (x0).\n"
"    # STR cannot have sp as a target register\n"
"    mov  x4, sp\n"
"    str  x4, [x0]\n"
"    # restore RSP (pointing to context-data) from A2 (x1)\n"
"    mov  sp, x1\n"
"#    # test if fpu env should be preserved\n"
"#    cmp  w3, #0\n"
"#    b.eq  2f\n"
"    # load d8 - d15\n"
"    ldp  d8,  d9,  [sp, #0x00]\n"
"    ldp  d10, d11, [sp, #0x10]\n"
"    ldp  d12, d13, [sp, #0x20]\n"
"    ldp  d14, d15, [sp, #0x30]\n"
"2:\n"
"    # load x19-x30\n"
"    ldp  x19, x20, [sp, #0x40]\n"
"    ldp  x21, x22, [sp, #0x50]\n"
"    ldp  x23, x24, [sp, #0x60]\n"
"    ldp  x25, x26, [sp, #0x70]\n"
"    ldp  x27, x28, [sp, #0x80]\n"
"    ldp  x29, x30, [sp, #0x90]\n"
"    # use third arg as return value after jump\n"
"    # and as first arg in context function\n"
"    mov  x0, x2\n"
"    # load pc\n"
"    ldr  x4, [sp, #0xa0]\n"
"    # restore stack from GP + FPU\n"
"    add  sp, sp, #0xb0\n"
"    ret x4\n"
".size   jump_fcontext,.-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_arm64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".cpu    generic+fp+simd\n"
".text\n"
".align  2\n"
".global make_fcontext\n"
".type   make_fcontext, %function\n"
"make_fcontext:\n"
#if defined(__ARM_FEATURE_BTI_DEFAULT) && (__ARM_FEATURE_BTI_DEFAULT == 1)
"    # bti c\n"
"    hint #34\n"
#endif
"    # shift address in x0 (allocated stack) to lower 16 byte boundary\n"
"    and x0, x0, ~0xF\n"
"    # reserve space for context-data on context-stack\n"
"    sub  x0, x0, #0xb0\n"
"    # third arg of make_fcontext() == address of context-function\n"
"    # store address as a PC to jump in\n"
"    str  x2, [x0, #0xa0]\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns (LR register)\n"
"    adr  x1, finish\n"
"    str  x1, [x0, #0x98]\n"
"    ret  x30 \n"
"finish:\n"
"    # exit code is zero\n"
"    mov  x0, #0\n"
"    # exit application\n"
"    bl  _exit\n"
".size   make_fcontext,.-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_loong64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".align 2\n"
".global jump_fcontext\n"
".type jump_fcontext,@function\n"
"jump_fcontext:\n"
"    # reserve space on stack\n"
"    addi.d      $sp, $sp, -176\n"
"    # save fs0-fs7\n"
"    fst.d       $fs0, $sp, 0\n"
"    fst.d       $fs1, $sp, 8\n"
"    fst.d       $fs2, $sp, 16\n"
"    fst.d       $fs3, $sp, 24\n"
"    fst.d       $fs4, $sp, 32\n"
"    fst.d       $fs5, $sp, 40\n"
"    fst.d       $fs6, $sp, 48\n"
"    fst.d       $fs7, $sp, 56\n"
"    # save s0-s8\n"
"    st.d        $s0, $sp, 64\n"
"    st.d        $s1, $sp, 72\n"
"    st.d        $s2, $sp, 80\n"
"    st.d        $s3, $sp, 88\n"
"    st.d        $s4, $sp, 96\n"
"    st.d        $s5, $sp, 104\n"
"    st.d        $s6, $sp, 112\n"
"    st.d        $s7, $sp, 120\n"
"    st.d        $s8, $sp, 128\n"
"    # save fp and ra\n"
"    st.d        $fp, $sp, 136\n"
"    st.d        $ra, $sp, 144\n"
"    # save ra as pc\n"
"    st.d        $ra, $sp, 152\n"
"    # store SP (pointing to old context-data) in pointer a0(first arg)\n"
"    st.d        $sp, $a0, 0\n"
"    # get SP (pointing to new context-data) from a1 param\n"
"    move        $sp, $a1\n"
"    # restore fs0-fs7\n"
"    fld.d       $fs0, $sp, 0\n"
"    fld.d       $fs1, $sp, 8\n"
"    fld.d       $fs2, $sp, 16\n"
"    fld.d       $fs3, $sp, 24\n"
"    fld.d       $fs4, $sp, 32\n"
"    fld.d       $fs5, $sp, 40\n"
"    fld.d       $fs6, $sp, 48\n"
"    fld.d       $fs7, $sp, 56\n"
"    # restore s0-s8\n"
"    ld.d        $s0, $sp, 64\n"
"    ld.d        $s1, $sp, 72\n"
"    ld.d        $s2, $sp, 80\n"
"    ld.d        $s3, $sp, 88\n"
"    ld.d        $s4, $sp, 96\n"
"    ld.d        $s5, $sp, 104\n"
"    ld.d        $s6, $sp, 112\n"
"    ld.d        $s7, $sp, 120\n"
"    ld.d        $s8, $sp, 128\n"
"    # restore fp and ra\n"
"    ld.d        $fp, $sp, 136\n"
"    ld.d        $ra, $sp, 144\n"
"    # load pc\n"
"    ld.d        $t0, $sp, 152\n"
"    st.d        $a2, $sp, 160\n"
"    # adjust stack\n"
"    addi.d      $sp, $sp, 176\n"
"    # move *data from a2 to a0 as param\n"
"    move        $a0, $a2\n"
"    # move *data from a2 to v0 as return\n"
"    move        $v0, $a2\n"
"    # jump to context\n"
"    jr          $t0\n"
".size jump_fcontext, .-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_loong64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".align 2\n"
".global make_fcontext\n"
".type make_fcontext,@function\n"
"make_fcontext:\n"
"    # shift address in $a0 (allocated stack) to lower 16 byte boundary\n"
"    addi.d      $v1, $zero, -16\n"
"    and         $v0, $v1, $a0\n"
"    # reserve space for context-data on context-stack\n"
"    addi.d      $v0, $v0, -176\n"
"    # third arg of make_fontext() == address of context-function\n"
"    st.d        $a2, $v0, 152\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns (LR register)\n"
"    la.local    $t0, finish\n"
"    st.d        $t0, $v0, 0x0090\n"
"    # return pointer to context-data\n"
"    jr          $ra\n"
"finish:\n"
"    # exit code is zero\n"
"    ext.w.h     $a0, $zero\n"
"    # exit application\n"
"    bl          _exit\n"
".size make_fcontext, .-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_mips_n64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl jump_fcontext\n"
".align 2\n"
".set noreorder\n"
".type jump_fcontext,@function\n"
".ent jump_fcontext\n"
"jump_fcontext:\n"
"    # reserve space on stack\n"
"    daddiu $sp, $sp, -176\n"
"    sd  $s0, 64($sp)  # save S0\n"
"    sd  $s1, 72($sp)  # save S1\n"
"    sd  $s2, 80($sp)  # save S2\n"
"    sd  $s3, 88($sp)  # save S3\n"
"    sd  $s4, 96($sp)  # save S4\n"
"    sd  $s5, 104($sp) # save S5\n"
"    sd  $s6, 112($sp) # save S6\n"
"    sd  $s7, 120($sp) # save S7\n"
"    sd  $fp, 128($sp) # save FP\n"
"    sd  $ra, 144($sp) # save RA\n"
"    sd  $ra, 152($sp) # save RA as PC\n"
"    s.d  $f24, 0($sp)   # save F24\n"
"    s.d  $f25, 8($sp)   # save F25\n"
"    s.d  $f26, 16($sp)  # save F26\n"
"    s.d  $f27, 24($sp)  # save F27\n"
"    s.d  $f28, 32($sp)  # save F28\n"
"    s.d  $f29, 40($sp)  # save F29\n"
"    s.d  $f30, 48($sp)  # save F30\n"
"    s.d  $f31, 56($sp)  # save F31\n"
"    # store SP (pointing to old context-data) in pointer a0(first arg)\n"
"    sd  $sp, 0($a0)\n"
"    # get SP (pointing to new context-data) from a1 param\n"
"    move  $sp, $a1\n"
"    l.d  $f24, 0($sp)   # restore F24\n"
"    l.d  $f25, 8($sp)   # restore F25\n"
"    l.d  $f26, 16($sp)  # restore F26\n"
"    l.d  $f27, 24($sp)  # restore F27\n"
"    l.d  $f28, 32($sp)  # restore F28\n"
"    l.d  $f29, 40($sp)  # restore F29\n"
"    l.d  $f30, 48($sp)  # restore F30\n"
"    l.d  $f31, 56($sp)  # restore F31\n"
"    ld  $s0, 64($sp)  # restore S0\n"
"    ld  $s1, 72($sp)  # restore S1\n"
"    ld  $s2, 80($sp)  # restore S2\n"
"    ld  $s3, 88($sp)  # restore S3\n"
"    ld  $s4, 96($sp)  # restore S4\n"
"    ld  $s5, 104($sp) # restore S5\n"
"    ld  $s6, 112($sp) # restore S6\n"
"    ld  $s7, 120($sp) # restore S7\n"
"    ld  $fp, 128($sp) # restore FP\n"
"    ld  $ra, 144($sp) # restore RA\n"
"    # load PC\n"
"    ld  $t9, 152($sp)\n"
"    sd  $a2, 160($sp)\n"
"    # adjust stack\n"
"    daddiu $sp, $sp, 176\n"
"    move  $a0, $a2 # move *data from a2 to a0 as param\n"
"    move  $v0, $a2 # move *data from a2 to v0 as return\n"
"    # jump to context\n"
"    jr  $t9\n"
"    nop\n"
".end jump_fcontext\n"
".size jump_fcontext, .-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_mips_n64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl make_fcontext\n"
".align 2\n"
".set noreorder\n"
".type make_fcontext,@function\n"
".ent make_fcontext\n"
"make_fcontext:\n"
"#ifdef __PIC__\n"
".set    noreorder\n"
".cpload $t9\n"
".set    reorder\n"
"#endif\n"
"    # shift address in A0 to lower 16 byte boundary\n"
"    li $v1, 0xfffffffffffffff0\n"
"    and $v0, $v1, $a0\n"
"    # reserve space for context-data on context-stack\n"
"    daddiu $v0, $v0, -176\n"
"    # third arg of make_fcontext() == address of context-function\n"
"    sd  $a2, 152($v0)\n"
"    # save global pointer in context-data\n"
"    sd  $gp, 136($v0)\n"
"    # psudo instruction compute abs address of label finish based on GP\n"
"    dla  $t9, finish\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns\n"
"    sd  $t9, 144($v0)\n"
"    jr  $ra # return pointer to context-data\n"
"    nop\n"
"finish:\n"
"    # reload our gp register (needed for la)\n"
"    daddiu $t0, $sp, -176\n"
"    ld $gp, 136($t0)\n"
"    ld $v0, 160($t0)\n"
"    # call _exit(0)\n"
"    dla $t9, _exit\n"
"    move $a0, $zero\n"
"    jr $t9\n"
"    nop\n"
".end make_fcontext\n"
".size make_fcontext, .-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_ppc32) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl jump_fcontext\n"
".align 2\n"
".type jump_fcontext,@function\n"
"jump_fcontext:\n"
"    # reserve space on stack\n"
"    subi  %r1, %r1, 240\n"
"    stw  %r13, 152(%r1)  # save R13\n"
"    stw  %r14, 156(%r1)  # save R14\n"
"    stw  %r15, 160(%r1)  # save R15\n"
"    stw  %r16, 164(%r1)  # save R16\n"
"    stw  %r17, 168(%r1)  # save R17\n"
"    stw  %r18, 172(%r1)  # save R18\n"
"    stw  %r19, 176(%r1)  # save R19\n"
"    stw  %r20, 180(%r1)  # save R20\n"
"    stw  %r21, 184(%r1)  # save R21\n"
"    stw  %r22, 188(%r1)  # save R22\n"
"    stw  %r23, 192(%r1)  # save R23\n"
"    stw  %r24, 196(%r1)  # save R24\n"
"    stw  %r25, 200(%r1)  # save R25\n"
"    stw  %r26, 204(%r1)  # save R26\n"
"    stw  %r27, 208(%r1)  # save R27\n"
"    stw  %r28, 212(%r1)  # save R28\n"
"    stw  %r29, 216(%r1)  # save R29\n"
"    stw  %r30, 220(%r1)  # save R30\n"
"    stw  %r31, 224(%r1)  # save R31\n"
"    # save CR\n"
"    mfcr  %r0\n"
"    stw  %r0, 228(%r1)\n"
"    # save LR\n"
"    mflr  %r0\n"
"    stw  %r0, 232(%r1)\n"
"    # save LR as PC\n"
"    stw  %r0, 236(%r1)\n"
"    # test if fpu env should be preserved\n"
"    cmpwi  cr7, %r6, 0\n"
"    beq  cr7, 1f\n"
"    stfd  %f14, 0(%r1)  # save F14\n"
"    stfd  %f15, 8(%r1)  # save F15\n"
"    stfd  %f16, 16(%r1)  # save F16\n"
"    stfd  %f17, 24(%r1)  # save F17\n"
"    stfd  %f18, 32(%r1)  # save F18\n"
"    stfd  %f19, 40(%r1)  # save F19\n"
"    stfd  %f20, 48(%r1)  # save F20\n"
"    stfd  %f21, 56(%r1)  # save F21\n"
"    stfd  %f22, 64(%r1)  # save F22\n"
"    stfd  %f23, 72(%r1)  # save F23\n"
"    stfd  %f24, 80(%r1)  # save F24\n"
"    stfd  %f25, 88(%r1)  # save F25\n"
"    stfd  %f26, 96(%r1)  # save F26\n"
"    stfd  %f27, 104(%r1)  # save F27\n"
"    stfd  %f28, 112(%r1)  # save F28\n"
"    stfd  %f29, 120(%r1)  # save F29\n"
"    stfd  %f30, 128(%r1)  # save F30\n"
"    stfd  %f31, 136(%r1)  # save F31\n"
"    mffs  %f0  # load FPSCR\n"
"    stfd  %f0, 144(%r1)  # save FPSCR\n"
"1:\n"
"    # store RSP (pointing to context-data) in R3\n"
"    stw  %r1, 0(%r3)\n"
"    # restore RSP (pointing to context-data) from R4\n"
"    mr  %r1, %r4\n"
"    # test if fpu env should be preserved\n"
"    cmpwi  cr7, %r6, 0\n"
"    beq  cr7, 2f\n"
"    lfd  %f14, 0(%r1)  # restore F14\n"
"    lfd  %f15, 8(%r1)  # restore F15\n"
"    lfd  %f16, 16(%r1)  # restore F16\n"
"    lfd  %f17, 24(%r1)  # restore F17\n"
"    lfd  %f18, 32(%r1)  # restore F18\n"
"    lfd  %f19, 40(%r1)  # restore F19\n"
"    lfd  %f20, 48(%r1)  # restore F20\n"
"    lfd  %f21, 56(%r1)  # restore F21\n"
"    lfd  %f22, 64(%r1)  # restore F22\n"
"    lfd  %f23, 72(%r1)  # restore F23\n"
"    lfd  %f24, 80(%r1)  # restore F24\n"
"    lfd  %f25, 88(%r1)  # restore F25\n"
"    lfd  %f26, 96(%r1)  # restore F26\n"
"    lfd  %f27, 104(%r1)  # restore F27\n"
"    lfd  %f28, 112(%r1)  # restore F28\n"
"    lfd  %f29, 120(%r1)  # restore F29\n"
"    lfd  %f30, 128(%r1)  # restore F30\n"
"    lfd  %f31, 136(%r1)  # restore F31\n"
"    lfd  %f0,  144(%r1)  # load FPSCR\n"
"    mtfsf  0xff, %f0  # restore FPSCR\n"
"2:\n"
"    lwz  %r13, 152(%r1)  # restore R13\n"
"    lwz  %r14, 156(%r1)  # restore R14\n"
"    lwz  %r15, 160(%r1)  # restore R15\n"
"    lwz  %r16, 164(%r1)  # restore R16\n"
"    lwz  %r17, 168(%r1)  # restore R17\n"
"    lwz  %r18, 172(%r1)  # restore R18\n"
"    lwz  %r19, 176(%r1)  # restore R19\n"
"    lwz  %r20, 180(%r1)  # restore R20\n"
"    lwz  %r21, 184(%r1)  # restore R21\n"
"    lwz  %r22, 188(%r1)  # restore R22\n"
"    lwz  %r23, 192(%r1)  # restore R23\n"
"    lwz  %r24, 196(%r1)  # restore R24\n"
"    lwz  %r25, 200(%r1)  # restore R25\n"
"    lwz  %r26, 204(%r1)  # restore R26\n"
"    lwz  %r27, 208(%r1)  # restore R27\n"
"    lwz  %r28, 212(%r1)  # restore R28\n"
"    lwz  %r29, 216(%r1)  # restore R29\n"
"    lwz  %r30, 220(%r1)  # restore R30\n"
"    lwz  %r31, 224(%r1)  # restore R31\n"
"    # restore CR\n"
"    lwz  %r0, 228(%r1)\n"
"    mtcr  %r0\n"
"    # restore LR\n"
"    lwz  %r0, 232(%r1)\n"
"    mtlr  %r0\n"
"    # load PC\n"
"    lwz  %r0, 236(%r1)\n"
"    # restore CTR\n"
"    mtctr  %r0\n"
"    # adjust stack\n"
"    addi  %r1, %r1, 240\n"
"    # use third arg as return value after jump\n"
"    # use third arg as first arg in context function\n"
"    mr  %r3, %r5\n"
"    # jump to context\n"
"    bctr\n"
".size jump_fcontext, .-jump_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_ppc32) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl make_fcontext\n"
".align 2\n"
".type make_fcontext,@function\n"
"make_fcontext:\n"
"    # save return address into R6\n"
"    mflr  %r6\n"
"    # first arg of make_fcontext() == top address of context-function\n"
"    # shift address in R3 to lower 16 byte boundary\n"
"    clrrwi  %r3, %r3, 4\n"
"    # reserve space for context-data on context-stack\n"
"    # including 64 byte of linkage + parameter area (R1 % 16 == 0)\n"
"    subi  %r3, %r3, 304\n"
"    # third arg of make_fcontext() == address of context-function\n"
"    stw  %r5, 236(%r3)\n"
"    # load LR\n"
"    mflr  %r0\n"
"    # jump to label 1\n"
"    bl  1f\n"
"1:\n"
"    # load LR into R4\n"
"    mflr  %r4\n"
"    # compute abs address of label finish\n"
"    addi  %r4, %r4, finish - 1b\n"
"    # restore LR\n"
"    mtlr  %r0\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns\n"
"    stw  %r4, 232(%r3)\n"
"    # restore return address from R6\n"
"    mtlr  %r6\n"
"    blr  # return pointer to context-data\n"
"finish:\n"
"    # save return address into R0\n"
"    mflr  %r0\n"
"    # save return address on stack, set up stack frame\n"
"    stw  %r0, 4(%r1)\n"
"    # allocate stack space, R1 % 16 == 0\n"
"    stwu  %r1, -16(%r1)\n"
"    # exit code is zero\n"
"    li  %r3, 0\n"
"    # exit application\n"
"    bl  _exit@plt\n"
".size make_fcontext, .-make_fcontext\n"
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_ppc64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".globl jump_fcontext\n"
#if _CALL_ELF == 2
"  .text\n"
"  .align 2\n"
"jump_fcontext:\n"
"        addis   %r2, %r12, .TOC.-jump_fcontext@ha\n"
"        addi    %r2, %r2, .TOC.-jump_fcontext@l\n"
"        .localentry jump_fcontext, . - jump_fcontext\n"
#else
"  .section \".opd\",\"aw\"\n"
"  .align 3\n"
"jump_fcontext:\n"
# ifdef _CALL_LINUX
"        .quad   .L.jump_fcontext,.TOC.@tocbase,0\n"
"        .type   jump_fcontext,@function\n"
"        .text\n"
"        .align 2\n"
".L.jump_fcontext:\n"
# else
"        .hidden .jump_fcontext\n"
"        .globl  .jump_fcontext\n"
"        .quad   .jump_fcontext,.TOC.@tocbase,0\n"
"        .size   jump_fcontext,24\n"
"        .type   .jump_fcontext,@function\n"
"        .text\n"
"        .align 2\n"
".jump_fcontext:\n"
# endif
#endif
"    # reserve space on stack\n"
"    subi  %r1, %r1, 320\n"
#if _CALL_ELF != 2
"    std  %r2,  144(%r1)  # save TOC\n"
#endif
"    std  %r14, 152(%r1)  # save R14\n"
"    std  %r15, 160(%r1)  # save R15\n"
"    std  %r16, 168(%r1)  # save R16\n"
"    std  %r17, 176(%r1)  # save R17\n"
"    std  %r18, 184(%r1)  # save R18\n"
"    std  %r19, 192(%r1)  # save R19\n"
"    std  %r20, 200(%r1)  # save R20\n"
"    std  %r21, 208(%r1)  # save R21\n"
"    std  %r22, 216(%r1)  # save R22\n"
"    std  %r23, 224(%r1)  # save R23\n"
"    std  %r24, 232(%r1)  # save R24\n"
"    std  %r25, 240(%r1)  # save R25\n"
"    std  %r26, 248(%r1)  # save R26\n"
"    std  %r27, 256(%r1)  # save R27\n"
"    std  %r28, 264(%r1)  # save R28\n"
"    std  %r29, 272(%r1)  # save R29\n"
"    std  %r30, 280(%r1)  # save R30\n"
"    std  %r31, 288(%r1)  # save R31\n"
"    # save CR\n"
"    mfcr  %r0\n"
"    std  %r0, 296(%r1)\n"
"    # save LR\n"
"    mflr  %r0\n"
"    std  %r0, 304(%r1)\n"
"    # save LR as PC\n"
"    std  %r0, 312(%r1)\n"
"    # test if fpu env should be preserved\n"
"    cmpwi  cr7, %r6, 0\n"
"    beq  cr7, 1f\n"
"    stfd  %f14, 0(%r1)  # save F14\n"
"    stfd  %f15, 8(%r1)  # save F15\n"
"    stfd  %f16, 16(%r1)  # save F16\n"
"    stfd  %f17, 24(%r1)  # save F17\n"
"    stfd  %f18, 32(%r1)  # save F18\n"
"    stfd  %f19, 40(%r1)  # save F19\n"
"    stfd  %f20, 48(%r1)  # save F20\n"
"    stfd  %f21, 56(%r1)  # save F21\n"
"    stfd  %f22, 64(%r1)  # save F22\n"
"    stfd  %f23, 72(%r1)  # save F23\n"
"    stfd  %f24, 80(%r1)  # save F24\n"
"    stfd  %f25, 88(%r1)  # save F25\n"
"    stfd  %f26, 96(%r1)  # save F26\n"
"    stfd  %f27, 104(%r1)  # save F27\n"
"    stfd  %f28, 112(%r1)  # save F28\n"
"    stfd  %f29, 120(%r1)  # save F29\n"
"    stfd  %f30, 128(%r1)  # save F30\n"
"    stfd  %f31, 136(%r1)  # save F31\n"
"1:\n"
"    # store RSP (pointing to context-data) in R3\n"
"    std  %r1, 0(%r3)\n"
"    # restore RSP (pointing to context-data) from R4\n"
"    mr  %r1, %r4\n"
"    # test if fpu env should be preserved\n"
"    cmpwi  cr7, %r6, 0\n"
"    beq  cr7, 2f\n"
"    lfd  %f14, 0(%r1)  # restore F14\n"
"    lfd  %f15, 8(%r1)  # restore F15\n"
"    lfd  %f16, 16(%r1)  # restore F16\n"
"    lfd  %f17, 24(%r1)  # restore F17\n"
"    lfd  %f18, 32(%r1)  # restore F18\n"
"    lfd  %f19, 40(%r1)  # restore F19\n"
"    lfd  %f20, 48(%r1)  # restore F20\n"
"    lfd  %f21, 56(%r1)  # restore F21\n"
"    lfd  %f22, 64(%r1)  # restore F22\n"
"    lfd  %f23, 72(%r1)  # restore F23\n"
"    lfd  %f24, 80(%r1)  # restore F24\n"
"    lfd  %f25, 88(%r1)  # restore F25\n"
"    lfd  %f26, 96(%r1)  # restore F26\n"
"    lfd  %f27, 104(%r1)  # restore F27\n"
"    lfd  %f28, 112(%r1)  # restore F28\n"
"    lfd  %f29, 120(%r1)  # restore F29\n"
"    lfd  %f30, 128(%r1)  # restore F30\n"
"    lfd  %f31, 136(%r1)  # restore F31\n"
"2:\n"
#if _CALL_ELF != 2
"    ld  %r2,  144(%r1)  # restore TOC\n"
#endif
"    ld  %r14, 152(%r1)  # restore R14\n"
"    ld  %r15, 160(%r1)  # restore R15\n"
"    ld  %r16, 168(%r1)  # restore R16\n"
"    ld  %r17, 176(%r1)  # restore R17\n"
"    ld  %r18, 184(%r1)  # restore R18\n"
"    ld  %r19, 192(%r1)  # restore R19\n"
"    ld  %r20, 200(%r1)  # restore R20\n"
"    ld  %r21, 208(%r1)  # restore R21\n"
"    ld  %r22, 216(%r1)  # restore R22\n"
"    ld  %r23, 224(%r1)  # restore R23\n"
"    ld  %r24, 232(%r1)  # restore R24\n"
"    ld  %r25, 240(%r1)  # restore R25\n"
"    ld  %r26, 248(%r1)  # restore R26\n"
"    ld  %r27, 256(%r1)  # restore R27\n"
"    ld  %r28, 264(%r1)  # restore R28\n"
"    ld  %r29, 272(%r1)  # restore R29\n"
"    ld  %r30, 280(%r1)  # restore R30\n"
"    ld  %r31, 288(%r1)  # restore R31\n"
"    # restore CR\n"
"    ld  %r0, 296(%r1)\n"
"    mtcr  %r0\n"
"    # restore LR\n"
"    ld  %r0, 304(%r1)\n"
"    mtlr  %r0\n"
"    # load PC\n"
"    ld  %r12, 312(%r1)\n"
"    # restore CTR\n"
"    mtctr  %r12\n"
"    # adjust stack\n"
"    addi  %r1, %r1, 320\n"
"    # use third arg as return value after jump\n"
"    # use third arg as first arg in context function\n"
"    mr  %r3, %r5\n"
"    # jump to context\n"
"    bctr\n"
#if _CALL_ELF == 2
"  .size jump_fcontext, .-jump_fcontext\n"
#else
# ifdef _CALL_LINUX
"  .size .jump_fcontext, .-.L.jump_fcontext\n"
# else
"  .size .jump_fcontext, .-.jump_fcontext\n"
# endif
#endif
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_ppc64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".globl make_fcontext\n"
#if _CALL_ELF == 2
"  .text\n"
"  .align 2\n"
"make_fcontext:\n"
"  addis   %r2, %r12, .TOC.-make_fcontext@ha\n"
"  addi    %r2, %r2, .TOC.-make_fcontext@l\n"
"  .localentry make_fcontext, . - make_fcontext\n"
#else
"  .section \".opd\",\"aw\"\n"
"  .align 3\n"
"make_fcontext:\n"
# ifdef _CALL_LINUX
"  .quad   .L.make_fcontext,.TOC.@tocbase,0\n"
"  .type   make_fcontext,@function\n"
"  .text\n"
"  .align 2\n"
".L.make_fcontext:\n"
# else
"  .hidden .make_fcontext\n"
"  .globl  .make_fcontext\n"
"  .quad   .make_fcontext,.TOC.@tocbase,0\n"
"  .size   make_fcontext,24\n"
"  .type   .make_fcontext,@function\n"
"  .text\n"
"  .align 2\n"
".make_fcontext:\n"
# endif
#endif
"    # save return address into R6\n"
"    mflr  %r6\n"
"    # first arg of make_fcontext() == top address of context-stack\n"
"    # shift address in R3 to lower 16 byte boundary\n"
"    clrrdi  %r3, %r3, 4\n"
"    # reserve space for context-data on context-stack\n"
"    # including 64 byte of linkage + parameter area (R1 % 16 == 0)\n"
"    subi  %r3, %r3, 384\n"
"    # third arg of make_fcontext() == address of context-function\n"
"    # entry point (ELFv2) or descriptor (ELFv1)\n"
#if _CALL_ELF == 2
"    # save address of context-function entry point\n"
"    std  %r5, 312(%r3)\n"
#else
"    # save address of context-function entry point\n"
"    ld   %r4, 0(%r5)\n"
"    std  %r4, 312(%r3)\n"
"    # save TOC of context-function\n"
"    ld   %r4, 8(%r5)\n"
"    std  %r4, 144(%r3)\n"
#endif
"    # load LR\n"
"    mflr  %r0\n"
"    # jump to label 1\n"
"    bl  1f\n"
"1:\n"
"    # load LR into R4\n"
"    mflr  %r4\n"
"    # compute abs address of label finish\n"
"    addi  %r4, %r4, finish - 1b\n"
"    # restore LR\n"
"    mtlr  %r0\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns\n"
"    std  %r4, 304(%r3)\n"
"    # restore return address from R6\n"
"    mtlr  %r6\n"
"    blr  # return pointer to context-data\n"
"finish:\n"
"    # save return address into R0\n"
"    mflr  %r0\n"
"    # save return address on stack, set up stack frame\n"
"    std  %r0, 8(%r1)\n"
"    # allocate stack space, R1 % 16 == 0\n"
"    stdu  %r1, -32(%r1)\n"
"    # exit code is zero\n"
"    li  %r3, 0\n"
"    # exit application\n"
"    bl  _exit\n"
"    nop\n"
#if _CALL_ELF == 2
"  .size make_fcontext, .-make_fcontext\n"
#else
# ifdef _CALL_LINUX
"  .size .make_fcontext, .-.L.make_fcontext\n"
# else
"  .size .make_fcontext, .-.make_fcontext\n"
# endif
#endif
);

#endif

#if defined(LIBCONTEXT_PLATFORM_linux_riscv64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".align  1\n"
".global jump_fcontext\n"
".type   jump_fcontext, %function\n"
"jump_fcontext:\n"
"    # prepare stack for GP + FPU\n"
"    addi  sp, sp, -0xd0\n"
"    # save fs0 - fs11\n"
"    fsd  fs0, 0x00(sp)\n"
"    fsd  fs1, 0x08(sp)\n"
"    fsd  fs2, 0x10(sp)\n"
"    fsd  fs3, 0x18(sp)\n"
"    fsd  fs4, 0x20(sp)\n"
"    fsd  fs5, 0x28(sp)\n"
"    fsd  fs6, 0x30(sp)\n"
"    fsd  fs7, 0x38(sp)\n"
"    fsd  fs8, 0x40(sp)\n"
"    fsd  fs9, 0x48(sp)\n"
"    fsd  fs10, 0x50(sp)\n"
"    fsd  fs11, 0x58(sp)\n"
"    # save s0-s11, ra\n"
"    sd  s0, 0x60(sp)\n"
"    sd  s1, 0x68(sp)\n"
"    sd  s2, 0x70(sp)\n"
"    sd  s3, 0x78(sp)\n"
"    sd  s4, 0x80(sp)\n"
"    sd  s5, 0x88(sp)\n"
"    sd  s6, 0x90(sp)\n"
"    sd  s7, 0x98(sp)\n"
"    sd  s8, 0xa0(sp)\n"
"    sd  s9, 0xa8(sp)\n"
"    sd  s10, 0xb0(sp)\n"
"    sd  s11, 0xb8(sp)\n"
"    sd  ra, 0xc0(sp)\n"
"    # save RA as PC\n"
"    sd  ra, 0xc8(sp)\n"
"    # store SP in the first arg(pointer to fcontext_t)\n"
"    sd  sp, 0x00(a0)\n"
"    # restore SP from the second arg(fcontext_t)\n"
"    mv  sp, a1\n"
"    # load fs0 - fs11\n"
"    fld  fs0, 0x00(sp)\n"
"    fld  fs1, 0x08(sp)\n"
"    fld  fs2, 0x10(sp)\n"
"    fld  fs3, 0x18(sp)\n"
"    fld  fs4, 0x20(sp)\n"
"    fld  fs5, 0x28(sp)\n"
"    fld  fs6, 0x30(sp)\n"
"    fld  fs7, 0x38(sp)\n"
"    fld  fs8, 0x40(sp)\n"
"    fld  fs9, 0x48(sp)\n"
"    fld  fs10, 0x50(sp)\n"
"    fld  fs11, 0x58(sp)\n"
"    # load s0-s11,ra\n"
"    ld  s0, 0x60(sp)\n"
"    ld  s1, 0x68(sp)\n"
"    ld  s2, 0x70(sp)\n"
"    ld  s3, 0x78(sp)\n"
"    ld  s4, 0x80(sp)\n"
"    ld  s5, 0x88(sp)\n"
"    ld  s6, 0x90(sp)\n"
"    ld  s7, 0x98(sp)\n"
"    ld  s8, 0xa0(sp)\n"
"    ld  s9, 0xa8(sp)\n"
"    ld  s10, 0xb0(sp)\n"
"    ld  s11, 0xb8(sp)\n"
"    ld  ra, 0xc0(sp)\n"
"    # use the third arg as return value\n"
"    mv a0, a2\n"
"    # load pc from new context\n"
"    ld  a4, 0xc8(sp)\n"
"    # restore stack from GP + FPU\n"
"    addi  sp, sp, 0xd0\n"
"    jr a4\n"
".size   jump_fcontext,.-jump_fcontext\n"
);
#endif

#if defined(LIBCONTEXT_PLATFORM_linux_riscv64) && defined(LIBCONTEXT_COMPILER_gcc)
__asm (
".text\n"
".align  1\n"
".global make_fcontext\n"
".type   make_fcontext, %function\n"
"make_fcontext:\n"
"    # shift address in a0 (allocated stack) to lower 16 byte boundary\n"
"    andi a0, a0, ~0xF\n"
"    # reserve space for context-data on context-stack\n"
"    addi  a0, a0, -0xd0\n"
"    # third arg of make_fcontext() == address of context-function\n"
"    # store address as a PC to jump in\n"
"    sd  a2, 0xc8(a0)\n"
"    # save address of finish as return-address for context-function\n"
"    # will be entered after context-function returns (RA register)\n"
"    lla  a4, finish\n"
"    sd  a4, 0xc0(a0)\n"
"    ret # return pointer to context-data (a0)\n"
"finish:\n"
"    # exit code is zero\n"
"    li  a0, 0\n"
"    # exit application\n"
"    tail  _exit@plt\n"
".size   make_fcontext,.-make_fcontext\n"
);
#endif


#ifdef __cplusplus
extern "C" {
#endif

namespace libcontext
{

#if !defined(LIBCONTEXT_PLATFORM_wasm32)
void LIBCONTEXT_CALL_CONVENTION release_fcontext( fcontext_t ctx )
{
	// do nothing...
}
#endif

}; // namespace libcontext

#ifdef __cplusplus
};
#endif
