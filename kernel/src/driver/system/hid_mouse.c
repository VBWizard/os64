// Bounded HID 1.11 report-descriptor interpreter for relative mice. Input
// offsets belong to report IDs, including padding and fields we do not use;
// Output and Feature items have separate streams and do not advance them.
#include "driver/system/hid_mouse.h"
#include "memset.h"

#define USAGE_MAX 32
#define LAYOUT_MAX 4
#define STACK_MAX 8

typedef struct {
    uint32_t page, size, count;
    int32_t min, max;
    uint8_t id;
} mouse_globals_t;

typedef struct {
    uint32_t usages[USAGE_MAX], minimum, maximum;
    unsigned count;
    bool has_min, has_max;
} mouse_locals_t;

static int32_t item_signed(uint32_t v, unsigned bytes)
{
    if (bytes == 1) return (int8_t)v;
    if (bytes == 2) return (int16_t)v;
    return (int32_t)v;
}

static uint32_t usage_at(const mouse_locals_t *l, unsigned n)
{
    if (n < l->count) return l->usages[n];
    if (l->has_min && l->has_max && l->maximum >= l->minimum) {
        n -= l->count;
        uint32_t distance = l->maximum - l->minimum;
        return l->minimum + (n < distance ? n : distance);
    }
    return l->count ? l->usages[l->count - 1] : 0;
}

static bool field_set(hid_mouse_field_t *f, uint16_t bit, uint32_t size)
{
    if (f->size || size == 0 || size > 16) return false;
    f->bit = bit;
    f->size = (uint8_t)size;
    return true;
}

bool hid_mouse_parse(const uint8_t *desc, size_t length, hid_mouse_layout_t *out)
{
    if (!desc || !out || !length || length > HID_MOUSE_DESCRIPTOR_BYTES) return false;
    mouse_globals_t g = {0}, globals[STACK_MAX];
    mouse_locals_t l = {0};
    bool collection[STACK_MAX];
    unsigned depth = 0, pushed = 0, layouts = 0;
    bool ids = false;
    uint16_t offsets[256] = {0};
    hid_mouse_layout_t candidates[LAYOUT_MAX] = {0};
    for (size_t pos = 0; pos < length;) {
        uint8_t prefix = desc[pos++];
        if (prefix == 0xfe) return false; // long items have no supported semantics
        unsigned size = prefix & 3u;
        if (size == 3) size = 4;
        if (size > length - pos) return false;
        uint32_t value = 0;
        for (unsigned i = 0; i < size; ++i) value |= (uint32_t)desc[pos++] << (8 * i);
        unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: if (value > 0xffff) return false; g.page = value; break;
            case 1: g.min = item_signed(value, size); break;
            case 2:
                if (g.min >= 0 && value > 0x7fffffff) return false;
                g.max = g.min < 0 ? item_signed(value, size) : (int32_t)value;
                break;
            case 3: case 4: case 5: case 6: break; // physical range and units
            case 7: g.size = value; break;
            case 8:
                if (!value || value > 255) return false;
                ids = true; g.id = (uint8_t)value; break;
            case 9: g.count = value; break;
            case 10:
                if (size || pushed == STACK_MAX) return false;
                globals[pushed++] = g; break;
            case 11:
                if (size || !pushed) return false;
                g = globals[--pushed]; break;
            default: return false;
            }
        } else if (type == 2) {
            uint32_t usage = size == 4 ? value : (g.page << 16) | value;
            switch (tag) {
            case 0:
                if (l.count == USAGE_MAX) return false;
                l.usages[l.count++] = usage; break;
            case 1: l.minimum = usage; l.has_min = true; break;
            case 2: l.maximum = usage; l.has_max = true; break;
            case 3: case 4: case 5: case 7: case 8: case 9: break;
            default: return false; // includes alternative usage delimiters
            }
        } else if (type == 0) {
            if (l.has_min != l.has_max || (l.has_min &&
                (l.maximum < l.minimum || (l.maximum >> 16) != (l.minimum >> 16)))) return false;
            if (tag == 10) {
                if (depth == STACK_MAX || size != 1) return false;
                bool mouse = value == 1 ? usage_at(&l, 0) == 0x10002 :
                             (depth && collection[depth - 1]);
                collection[depth++] = mouse;
            } else if (tag == 12) {
                if (!depth || size) return false;
                --depth;
            } else if (tag == 8) {
                if (!depth || !size || !g.size || !g.count ||
                    g.size > 512 || g.count > 512 || g.size * g.count > 512u - offsets[g.id]) return false;
                uint16_t start = offsets[g.id];
                offsets[g.id] += (uint16_t)(g.size * g.count);
                if (collection[depth - 1] && !(value & 1)) {
                    // Mouse controls must be variable fields, not usage arrays.
                    if (!(value & 2) || (value & 0x180)) return false;
                    for (unsigned i = 0; i < g.count; ++i) {
                        uint32_t usage = usage_at(&l, i);
                        bool button = usage >= 0x90001 && usage <= 0x90003;
                        bool axis = usage == 0x10030 || usage == 0x10031 || usage == 0x10038;
                        if (!button && !axis) continue;
                        unsigned n = 0;
                        while (n < layouts && candidates[n].report_id != g.id) ++n;
                        if (n == layouts) {
                            if (layouts == LAYOUT_MAX) return false;
                            candidates[layouts++].report_id = g.id;
                        }
                        hid_mouse_layout_t *m = &candidates[n];
                        hid_mouse_field_t *field;
                        if (button) {
                            if ((value & 4) || g.min != 0 || g.max != 1 || g.size != 1) return false;
                            field = &m->buttons[usage - 0x90001];
                        } else {
                            if (!(value & 4) || g.size > 16 || g.min >= 0 || g.max <= 0 ||
                                g.min < -(1 << (g.size - 1)) || g.max >= (1 << (g.size - 1))) return false;
                            field = usage == 0x10030 ? &m->x : usage == 0x10031 ? &m->y : &m->wheel;
                        }
                        if (!field_set(field, start + i * g.size, g.size)) return false;
                    }
                }
            } else if (tag != 9 && tag != 11) return false;
            memset(&l, 0, sizeof(l)); // local items expire at every Main item
        } else return false;
    }
    if (depth || pushed || (ids && offsets[0])) return false;
    uint16_t max_bytes = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (!offsets[i]) continue;
        uint16_t bytes = (offsets[i] + 7) / 8 + (ids ? 1 : 0);
        if (bytes > HID_MOUSE_REPORT_BYTES) return false;
        if (bytes > max_bytes) max_bytes = bytes;
    }
    hid_mouse_layout_t *chosen = NULL;
    for (unsigned i = 0; i < layouts; ++i) {
        hid_mouse_layout_t *m = &candidates[i];
        if (!m->x.size || !m->y.size || !m->wheel.size || !m->buttons[0].size) continue;
        if (chosen) return false;
        m->bytes = (offsets[m->report_id] + 7) / 8 + (ids ? 1 : 0);
        if (m->bytes > HID_MOUSE_REPORT_BYTES) return false;
        chosen = m;
    }
    if (!chosen) return false;
    chosen->max_bytes = max_bytes;
    *out = *chosen;
    return true;
}

static int16_t field_read(const uint8_t *data, hid_mouse_field_t f, bool sign)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < f.size; ++i)
        v |= (uint32_t)((data[(f.bit + i) / 8] >> ((f.bit + i) % 8)) & 1) << i;
    int32_t result = (int32_t)v;
    if (sign && f.size && (v & (1u << (f.size - 1)))) result -= (int32_t)(1u << f.size);
    return (int16_t)result;
}

bool hid_mouse_decode(const hid_mouse_layout_t *m, const uint8_t *report,
                      size_t length, hid_mouse_sample_t *out)
{
    if (!m || !report || !out || !m->bytes || length < m->bytes ||
        (m->report_id && report[0] != m->report_id)) return false;
    const uint8_t *data = report + (m->report_id ? 1 : 0);
    hid_mouse_sample_t s = {0};
    for (unsigned i = 0; i < 3; ++i)
        if (field_read(data, m->buttons[i], false)) s.buttons |= 1u << i;
    s.x = field_read(data, m->x, true);
    s.y = field_read(data, m->y, true);
    s.wheel = field_read(data, m->wheel, true);
    *out = s;
    return true;
}
