// Kernel objects, guest address -> object map and handle table.
//
// Semantics from Xenia's objects (https://github.com/xenia-project/xenia
// @ 95a5c3ee, BSD): src/xenia/kernel/xevent.cc, xsemaphore.cc, xmutant.cc,
// xtimer.cc, xobject.cc (GetNativeObject/StashHandle) and
// util/object_table.cc. The address -> object map follows Unleashed
// Recomp's idea (kernel/xdm.h, QueryKernelObject: signature in the WaitListHead), but
// the host object lives in a host table instead of the guest heap.
//
// Locks: the objects' state is under the dispatcher lock (sync.cpp);
// the address map, the handle table, Ob references and the mutant
// registry each have their own mutex. Order: dispatcher -> mutant
// registry. No shared_ptr is released while holding one of these locks
// (the destructors may take them): whoever removes an object from a table
// moves it into a variable declared before the lock.
#include "kernel/objects.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "kernel/guest.h"
#include "kernel/memory.h"
#include "kernel/registry.h"
#include "kernel/sync.h"
#include "log.h"

namespace kernel {
namespace {

using detail::DispatcherAccess;

// WaitListHead.Flink of a header that has a host object; Blink = entry id.
constexpr uint32_t kSignature = 0x4757324B;  // 'GW2K'

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", v);
  return buf;
}

struct MapEntry {
  uint32_t id = 0;
  std::shared_ptr<KernelObject> strong;  // objects born from the guest header
  std::weak_ptr<KernelObject> weak;      // associated objects (BindGuestObject)
  std::shared_ptr<KernelObject> Get() const { return strong ? strong : weak.lock(); }
};

// Global state never destroyed: objects still alive at exit must not
// find already-destroyed mutexes in their destructors.
struct Tables {
  std::mutex map_mutex;
  std::unordered_map<uint32_t, MapEntry> map;
  uint32_t next_id = 1;

  std::mutex handle_mutex;
  std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> handles;
  uint32_t next_handle = kFirstHandle;

  std::mutex ref_mutex;
  std::unordered_multimap<uint32_t, std::shared_ptr<KernelObject>> refs;

  std::mutex mutant_mutex;
  std::unordered_set<Mutant*> mutants;

  static constexpr uint32_t kFirstHandle = 0x00001000;
  static constexpr uint32_t kLastHandle = 0xFFFF0000;
};

Tables& T() {
  static Tables* t = new Tables;
  return *t;
}

DispatcherHeader* Header(uint32_t addr) { return GuestPtr<DispatcherHeader>(addr); }

// New object from the guest header state; nullptr for unhandled types.
std::shared_ptr<KernelObject> CreateFromHeader(uint32_t addr, const DispatcherHeader& h) {
  const int32_t state = h.signal_state;
  switch (h.type) {
    case kNotificationEvent:
    case kSynchronizationEvent:
      return std::make_shared<Event>(h.type == kNotificationEvent, state != 0);
    case kMutantObject:
      // SignalState <= 0: owned. The real owner is not known:
      // it becomes the current thread (KeInitializeMutant with InitialOwner).
      return std::make_shared<Mutant>(state <= 0);
    case kSemaphoreObject: {
      // KSEMAPHORE: header + Limit (Xenia X_KSEMAPHORE).
      const int32_t limit = *GuestPtr<be<int32_t>>(addr + sizeof(DispatcherHeader));
      return std::make_shared<Semaphore>(state, limit);
    }
    case kNotificationTimer:
    case kSynchronizationTimer:
      return std::make_shared<Timer>(h.type == kSynchronizationTimer);
    default:
      return nullptr;
  }
}

}  // namespace

// ---- KernelObject ----------------------------------------------------------

KernelObject::~KernelObject() {
  if (!owns_guest_object_) return;
  // header allocated by EnsureGuestObject: unlinked and returned to the heap
  const uint32_t addr = guest_object_.load();
  auto& t = T();
  {
    std::lock_guard g(t.map_mutex);
    auto it = t.map.find(addr);
    if (it != t.map.end() && !it->second.strong && it->second.weak.expired()) t.map.erase(it);
    if (DispatcherHeader* h = Header(addr); h && h->wait_list_flink == kSignature) {
      h->wait_list_flink = 0;
      h->wait_list_blink = 0;
    }
  }
  if (MemorySystem* m = Memory()) m->heap.Free(addr);
}

size_t KernelObject::WaiterCount() const {
  auto lock = LockDispatcher();
  return waiters_.size();
}

// ---- Event (Xenia XEvent) ----------------------------------------------------

Event::Event(bool manual_reset, bool initial_state) : manual_reset_(manual_reset), signaled_(initial_state) {}

int32_t Event::SetLocked() {
  const int32_t prev = signaled_;
  signaled_ = true;
  SatisfyWaiters(*this);
  return prev;
}

int32_t Event::Set() {
  auto lock = LockDispatcher();
  return SetLocked();
}

int32_t Event::Reset() {
  auto lock = LockDispatcher();
  const int32_t prev = signaled_;
  signaled_ = false;
  return prev;
}

// KePulseEvent: waits already registered are satisfied (all for a
// manual event, one for an automatic one), then the event stays not signaled.
int32_t Event::Pulse() {
  auto lock = LockDispatcher();
  const int32_t prev = signaled_;
  signaled_ = true;
  SatisfyWaiters(*this);
  signaled_ = false;
  return prev;
}

void Event::Acquire() {
  if (!manual_reset_) signaled_ = false;
}

// ---- Semaphore (Xenia XSemaphore) -------------------------------------------------

Semaphore::Semaphore(int32_t count, int32_t limit) : count_(count), limit_(limit) {}

bool Semaphore::ReleaseLocked(int32_t adjustment, int32_t* previous) {
  if (previous) *previous = count_;
  if (adjustment <= 0 || count_ > limit_ - adjustment) return false;
  count_ += adjustment;
  SatisfyWaiters(*this);
  return true;
}

bool Semaphore::Release(int32_t adjustment, int32_t* previous) {
  auto lock = LockDispatcher();
  return ReleaseLocked(adjustment, previous);
}

// ---- Mutant (Xenia XMutant, semantica KMUTANT) --------------------------------------

Mutant::Mutant(bool initial_owner) {
  if (initial_owner) {
    owner_ = CurrentThreadId();
    signal_state_ = 0;
  }
  auto& t = T();
  std::lock_guard g(t.mutant_mutex);
  t.mutants.insert(this);
}

Mutant::~Mutant() {
  auto& t = T();
  std::lock_guard g(t.mutant_mutex);
  t.mutants.erase(this);
}

bool Mutant::IsSignaled() const { return signal_state_ > 0 || owner_ == ActingThreadId(); }

void Mutant::Acquire() {
  owner_ = ActingThreadId();
  --signal_state_;
  abandoned_ = false;
}

uint32_t Mutant::ReleaseLocked(bool abandon, int32_t* previous) {
  if (previous) *previous = signal_state_;
  if (abandon) {
    signal_state_ = 1;
    owner_ = 0;
    abandoned_ = true;
    SatisfyWaiters(*this);
    return kStatusSuccess;
  }
  if (owner_ == 0 || owner_ != ActingThreadId()) return kStatusMutantNotOwned;
  if (++signal_state_ == 1) {
    owner_ = 0;
    SatisfyWaiters(*this);
  }
  return kStatusSuccess;
}

uint32_t Mutant::Release(bool abandon, int32_t* previous) {
  auto lock = LockDispatcher();
  return ReleaseLocked(abandon, previous);
}

uint32_t Mutant::Owner() const {
  auto lock = LockDispatcher();
  return owner_;
}

int32_t Mutant::SignalState() const {
  auto lock = LockDispatcher();
  return signal_state_;
}

// Lock order: dispatcher -> registry. The registry keeps the
// pointer alive (a mutant's destructor waits for its mutex), and
// SatisfyWaiters neither takes the registry nor releases objects.
void AbandonMutantsOwnedBy(uint32_t thread_id) {
  if (!thread_id) return;
  auto lock = LockDispatcher();
  auto& t = T();
  std::lock_guard g(t.mutant_mutex);
  for (Mutant* m : t.mutants) {
    if (m->owner_ != thread_id) continue;
    m->owner_ = 0;
    m->signal_state_ = 1;
    m->abandoned_ = true;
    SatisfyWaiters(*m);
  }
}

// ---- Timer (Xenia XTimer) -----------------------------------------------------

Timer::Timer(bool synchronization) : synchronization_(synchronization) {}

Timer::~Timer() {
  auto lock = LockDispatcher();
  detail::TimerDestroyed(*this);
}

bool Timer::Set(int64_t due, uint32_t period_ms, uint32_t routine, uint32_t routine_arg) {
  const int64_t due_ns = detail::DeadlineNs(due);
  const uint32_t thread = CurrentThreadId();
  auto lock = LockDispatcher();
  const bool was_active = active_;
  signaled_ = false;
  active_ = true;
  due_ns_ = due_ns;
  period_ns_ = int64_t(period_ms) * 1'000'000;
  routine_ = routine;
  routine_arg_ = routine_arg;
  apc_thread_ = thread;
  detail::TimerChanged(*this);
  return was_active;
}

bool Timer::Cancel() {
  auto lock = LockDispatcher();
  const bool was_active = active_;
  active_ = false;
  detail::TimerChanged(*this);
  return was_active;
}

bool Timer::SignalState() const {
  auto lock = LockDispatcher();
  return signaled_;
}

// ---- guest address -> object map (Xenia XObject::GetNativeObject) ------------------

std::shared_ptr<KernelObject> ObjectFromGuest(uint32_t guest_header) {
  if (!guest_header) return nullptr;
  DispatcherHeader* h = Header(guest_header);
  auto& t = T();
  std::shared_ptr<KernelObject> dropped;  // released after the lock
  std::lock_guard g(t.map_mutex);
  auto it = t.map.find(guest_header);
  if (it != t.map.end()) {
    if (h->wait_list_flink == kSignature && h->wait_list_blink == it->second.id) {
      auto obj = it->second.Get();
      if (obj && obj->GuestType() == h->type) return obj;
      dropped = std::move(obj);
    }
  }
  auto obj = CreateFromHeader(guest_header, *h);
  if (!obj) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 200)
      LogLine("[kernel] guest object " + Hex(guest_header) + " of type " + std::to_string(h->type) +
              " unsupported (caller " + Hex(ExportCaller()) + ")");
    return nullptr;
  }
  MapEntry& e = t.map[guest_header];
  if (!dropped) dropped = std::move(e.strong);
  e.strong = obj;
  e.weak.reset();
  e.id = t.next_id++;
  if (t.next_id == 0) t.next_id = 1;
  h->wait_list_flink = kSignature;
  h->wait_list_blink = e.id;
  DispatcherAccess::GuestObject(*obj) = guest_header;
  return obj;
}

void ResetGuestObject(uint32_t guest_header) {
  if (!guest_header) return;
  auto& t = T();
  std::shared_ptr<KernelObject> dropped;
  std::lock_guard g(t.map_mutex);
  if (auto it = t.map.find(guest_header); it != t.map.end()) {
    dropped = std::move(it->second.strong);
    t.map.erase(it);
  }
  // Empty WaitListHead, like InitializeListHead: no signature.
  const uint32_t list = guest_header + offsetof(DispatcherHeader, wait_list_flink);
  DispatcherHeader* h = Header(guest_header);
  h->wait_list_flink = list;
  h->wait_list_blink = list;
}

void BindGuestObject(uint32_t guest_header, const std::shared_ptr<KernelObject>& obj) {
  if (!guest_header || !obj) return;
  auto& t = T();
  std::shared_ptr<KernelObject> dropped;
  std::lock_guard g(t.map_mutex);
  MapEntry& e = t.map[guest_header];
  dropped = std::move(e.strong);
  e.weak = obj;
  e.id = t.next_id++;
  if (t.next_id == 0) t.next_id = 1;
  DispatcherHeader* h = Header(guest_header);
  h->wait_list_flink = kSignature;
  h->wait_list_blink = e.id;
  uint32_t expected = 0;
  DispatcherAccess::GuestObject(*obj).compare_exchange_strong(expected, guest_header);
}

uint32_t EnsureGuestObject(const std::shared_ptr<KernelObject>& obj) {
  if (!obj) return 0;
  if (const uint32_t a = obj->guest_object()) return a;
  MemorySystem* m = Memory();
  if (!m) return 0;
  auto& t = T();
  std::lock_guard g(t.map_mutex);
  if (const uint32_t a = obj->guest_object()) return a;
  const uint32_t addr = m->heap.Alloc(sizeof(DispatcherHeader));
  if (!addr) return 0;
  DispatcherHeader* h = Header(addr);
  *h = {};
  h->type = obj->GuestType();
  h->size = sizeof(DispatcherHeader) / 4;
  MapEntry& e = t.map[addr];
  e.strong.reset();  // freshly allocated address: no valid entry
  e.weak = obj;
  e.id = t.next_id++;
  if (t.next_id == 0) t.next_id = 1;
  h->wait_list_flink = kSignature;
  h->wait_list_blink = e.id;
  DispatcherAccess::GuestObject(*obj) = addr;
  DispatcherAccess::OwnsGuestObject(*obj) = true;
  return addr;
}

// ---- handle table (Xenia util::ObjectTable) --------------------------------------

uint32_t HandleCreate(std::shared_ptr<KernelObject> obj) {
  if (!obj) return 0;
  auto& t = T();
  std::lock_guard g(t.handle_mutex);
  uint32_t h;
  do {
    h = t.next_handle;
    t.next_handle += 4;
    if (t.next_handle >= Tables::kLastHandle) t.next_handle = Tables::kFirstHandle;
  } while (t.handles.contains(h));
  t.handles.emplace(h, std::move(obj));
  return h;
}

std::shared_ptr<KernelObject> HandleGet(uint32_t handle) {
  // Xenia ObjectTable::TranslateHandle (kernel/util/object_table.cc):
  // 0xFFFFFFFE = current thread.
  if (handle == kCurrentThreadHandle) return CurrentThreadObject();
  auto& t = T();
  std::lock_guard g(t.handle_mutex);
  auto it = t.handles.find(handle);
  return it == t.handles.end() ? nullptr : it->second;
}

bool HandleClose(uint32_t handle) {
  auto& t = T();
  std::shared_ptr<KernelObject> dropped;
  std::lock_guard g(t.handle_mutex);
  auto it = t.handles.find(handle);
  if (it == t.handles.end()) return false;
  dropped = std::move(it->second);
  t.handles.erase(it);
  return true;
}

// ---- references ObReferenceObjectByHandle / ObDereferenceObject ----------------------

void ObjectReference(uint32_t guest_object, std::shared_ptr<KernelObject> obj) {
  auto& t = T();
  std::lock_guard g(t.ref_mutex);
  t.refs.emplace(guest_object, std::move(obj));
}

bool ObjectDereference(uint32_t guest_object) {
  auto& t = T();
  std::shared_ptr<KernelObject> dropped;
  std::lock_guard g(t.ref_mutex);
  auto it = t.refs.find(guest_object);
  if (it == t.refs.end()) return false;
  dropped = std::move(it->second);
  t.refs.erase(it);
  return true;
}

}  // namespace kernel
