// Minimal stand-in for the parts of MAME's emu.h used by src/devices/cpu/sm510.
// Test-only: lets the unmodified MAME 0.289 SM5xx sources run as a reference model.
#ifndef LCD_REFERENCE_EMU_H
#define LCD_REFERENCE_EMU_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint32_t offs_t;

template <int N, typename T, typename... B> constexpr T bitswap(T v, B... b)
{
    static_assert(sizeof...(B) == N, "bit count");
    T out = 0;
    for (int bit : {int(b)...})
        out = (T)(out << 1 | ((v >> bit) & 1));
    return out;
}

#define ATTR_COLD
#define BIT(x, n) (((x) >> (n)) & 1)
#define NAME(x) x, #x
#define FUNC(x) &x, #x
#define INPUT_LINE_RESET 1000
#define STATE_GENPC (-1)
#define STATE_GENPCBASE (-2)
#define STATE_GENFLAGS (-3)
#define ENDIANNESS_LITTLE 0
#define AS_PROGRAM 0
#define AS_DATA 1
#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)

class device_t;
class machine_config {};
typedef const char *device_type;
#define DECLARE_DEVICE_TYPE(Type, Class) extern device_type const Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) device_type const Type = Short;

namespace util {
class disasm_interface {
public:
    virtual ~disasm_interface() = default;
};
}

struct attotime {
    u64 ticks = 0;
    static attotime from_ticks(u64 t, u32) { attotime a; a.ticks = t; return a; }
};

class emu_timer {
public:
    std::function<void(s32)> cb;
    u64 period = 0;
    void adjust(const attotime &start, s32 = 0, const attotime &per = attotime()) { (void)start; period = per.ticks; }
};

class address_map;
class address_map_constructor {
public:
    std::function<void(address_map &)> fn;
    address_map_constructor() = default;
    template <typename T, typename U> address_map_constructor(void (T::*f)(address_map &), const char *, U *self)
        : fn([self, f](address_map &m) { (static_cast<T *>(self)->*f)(m); }) {}
    void operator()(address_map &m) const { if (fn) fn(m); }
};

class address_map_entry {
public:
    offs_t start, end, m_mirror = 0;
    int kind = 0; /* 0 none, 1 rom, 2 ram, 3 handler */
    std::string m_share;
    std::vector<u8> m_ram;
    std::function<u8(offs_t)> rd;
    std::function<void(offs_t, u8)> wr;
    address_map_entry(offs_t s, offs_t e) : start(s), end(e) {}
    address_map_entry &rom() { kind = 1; return *this; }
    address_map_entry &ram() { kind = 2; return *this; }
    address_map_entry &mirror(offs_t m) { m_mirror = m; return *this; }
    address_map_entry &share(const char *n) { m_share = n; return *this; }
    template <typename T> address_map_entry &rw(u8 (T::*r)(offs_t), const char *, void (T::*w)(offs_t, u8), const char *);
    device_t *owner = nullptr;
    bool matches(offs_t a) const { a &= ~m_mirror; return a >= start && a <= end; }
};

class address_map {
public:
    device_t *owner = nullptr;
    std::vector<std::unique_ptr<address_map_entry>> entries;
    address_map_entry &operator()(offs_t s, offs_t e)
    {
        entries.emplace_back(new address_map_entry(s, e));
        entries.back()->owner = owner;
        return *entries.back();
    }
};

template <typename T>
address_map_entry &address_map_entry::rw(u8 (T::*r)(offs_t), const char *, void (T::*w)(offs_t, u8), const char *)
{
    kind = 3;
    T *dev = static_cast<T *>(owner);
    rd = [dev, r](offs_t o) { return (dev->*r)(o); };
    wr = [dev, w](offs_t o, u8 d) { (dev->*w)(o, d); };
    return *this;
}

class address_space_config {
public:
    const char *name;
    int width;
    address_map_constructor ctor;
    template <typename... A>
    address_space_config(const char *n, int, int, int w, int, address_map_constructor c) : name(n), width(w), ctor(c) {}
};

class address_space {
public:
    address_map map;
    std::vector<u8> *rom = nullptr;
    address_map_entry *find(offs_t a)
    {
        for (size_t i = map.entries.size(); i-- > 0;)
            if (map.entries[i]->kind && map.entries[i]->matches(a))
                return map.entries[i].get();
        return nullptr;
    }
    u8 read_byte(offs_t a)
    {
        address_map_entry *e = find(a);
        if (!e) return 0;
        offs_t o = (a & ~e->m_mirror);
        if (e->kind == 1) return rom && o < rom->size() ? (*rom)[o] : 0;
        if (e->kind == 2) return e->m_ram[o - e->start];
        return e->rd(a);
    }
    void write_byte(offs_t a, u8 d)
    {
        address_map_entry *e = find(a);
        if (!e) return;
        offs_t o = (a & ~e->m_mirror);
        if (e->kind == 2) e->m_ram[o - e->start] = d;
        else if (e->kind == 3) e->wr(a, d);
    }
};

typedef std::vector<std::pair<int, const address_space_config *>> space_config_vector;
namespace device_memory_interface { typedef ::space_config_vector space_config_vector; }

struct state_entry {
    state_entry &formatstr(const char *) { return *this; }
    state_entry &noshow() { return *this; }
};

template <typename T> class optional_shared_ptr {
public:
    T *ptr = nullptr;
    size_t size = 0;
    template <typename D> optional_shared_ptr(D &, const char *n) : name(n) {}
    const char *name;
    operator T *() const { return ptr; }
    size_t bytes() const { return size; }
};

template <typename T> class optional_region_ptr {
public:
    T *ptr = nullptr;
    template <typename D> optional_region_ptr(D &, const char *) {}
    operator T *() const { return ptr; }
};

class devcb_read8 {
public:
    std::function<u8()> fn;
    u8 def;
    template <typename D> devcb_read8(D &, u8 d) : def(d) {}
    devcb_read8 &bind() { return *this; }
    template <typename F> void set(F f) { fn = f; }
    u8 operator()() { return fn ? fn() : def; }
};

class devcb_read_line {
public:
    std::function<int()> fn;
    int def;
    template <typename D> devcb_read_line(D &, int d) : def(d) {}
    devcb_read_line &bind() { return *this; }
    template <typename F> void set(F f) { fn = f; }
    int operator()() { return fn ? fn() : def; }
};

class devcb_write8 {
public:
    std::function<void(u8)> fn;
    template <typename D> devcb_write8(D &) {}
    devcb_write8 &bind() { return *this; }
    template <typename F> void set(F f) { fn = f; }
    void operator()(u8 d) { if (fn) fn(d); }
};

class devcb_write16 {
public:
    std::function<void(offs_t, u16)> fn;
    template <typename D> devcb_write16(D &) {}
    devcb_write16 &bind() { return *this; }
    template <typename F> void set(F f) { fn = f; }
    void operator()(offs_t o, u16 d) { if (fn) fn(o, d); }
};

class device_t {
public:
    virtual ~device_t() = default;
};

class cpu_device : public device_t {
public:
    cpu_device(const machine_config &, device_type, const char *, device_t *, u32 clock) : m_clock(clock) {}
    u32 m_clock;
    int *m_icountptr = nullptr;
    address_space m_spaces[2];
    std::vector<emu_timer *> timers;
    u32 unscaled_clock() const { return m_clock; }
    address_space &space(int n) { return m_spaces[n]; }
    void set_icountptr(int &c) { m_icountptr = &c; }
    template <typename T> void save_item(T &, const char *) {}
    template <typename T> state_entry state_add(int, const char *, T &) { return state_entry(); }
    void standard_irq_callback(int, offs_t) {}
    void debugger_instruction_hook(offs_t) {}
    void debugger_wait_hook() {}
    void notify_clock_changed() {}
    template <typename... A> void logerror(const char *, A...) {}
    template <typename T> emu_timer *timer_alloc(void (T::*f)(s32), const char *, T *self)
    {
        emu_timer *t = new emu_timer();
        t->cb = [self, f](s32 p) { (self->*f)(p); };
        timers.push_back(t);
        return t;
    }
    virtual void device_start() {}
    virtual void device_reset() {}
    virtual void device_post_load() {}
    virtual u64 execute_clocks_to_cycles(u64 c) const noexcept { return c; }
    virtual u64 execute_cycles_to_clocks(u64 c) const noexcept { return c; }
    virtual u32 execute_min_cycles() const noexcept { return 1; }
    virtual u32 execute_max_cycles() const noexcept { return 1; }
    virtual void execute_set_input(int, int) {}
    virtual void execute_run() {}
    virtual space_config_vector memory_space_config() const { return space_config_vector(); }
    virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }
};

#endif
