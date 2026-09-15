// Exact decimal grid arithmetic for Range state. Shortest binary64 strings
// need at most 17 significant digits and exponents from -324 through 308.
// Aligning their coefficients, including a midpoint, fits in 80 base-1e9
// limbs. Storage and division work are bounded independently of input length.
#include "internal.h"
#include "os64/fmt.h"

#define R_WORDS 80
#define R_BASE 1000000000u
_Static_assert(R_WORDS*9 >= 660, "range decimal capacity includes intermediate products");

typedef struct {
    uint32_t word[R_WORDS]; // little endian base 1e9
    int used;
    bool negative;
    int exponent;
} RDecimal;

static void tidy(RDecimal *a)
{
    while (a->used != 0 && a->word[a->used-1] == 0)
        a->used--;
    if (a->used == 0)
        a->negative = false;
}

static int magnitude(const RDecimal *a, const RDecimal *b)
{
    if (a->used != b->used)
        return a->used < b->used ? -1 : 1;
    for (int i = a->used-1; i >= 0; i--)
        if (a->word[i] != b->word[i])
            return a->word[i] < b->word[i] ? -1 : 1;
    return 0;
}

static int compare(const RDecimal *a, const RDecimal *b)
{
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    int cmp = magnitude(a,b);
    return a->negative ? -cmp : cmp;
}

static void multiply(RDecimal *a, uint32_t by)
{
    uint64_t carry = 0;
    for (int i = 0; i < a->used; i++) {
        uint64_t product = (uint64_t)a->word[i] * by + carry;
        a->word[i] = (uint32_t)(product % R_BASE);
        carry = product / R_BASE;
    }
    if (carry != 0)
        a->word[a->used++] = (uint32_t)carry;
    tidy(a);
}

static void small_add(RDecimal *a, uint32_t digit)
{
    uint64_t carry = digit;
    for (int i = 0; carry != 0; i++) {
        if (i == a->used)
            a->word[a->used++] = 0;
        carry += a->word[i];
        a->word[i] = (uint32_t)(carry % R_BASE);
        carry /= R_BASE;
    }
}

// Magnitude subtraction, with a >= b.
static void subtract_magnitude(RDecimal *a, const RDecimal *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < a->used; i++) {
        int64_t value = (int64_t)a->word[i] - (i < b->used ? b->word[i] : 0) - borrow;
        borrow = value < 0;
        a->word[i] = (uint32_t)(value + (borrow ? R_BASE : 0));
    }
    tidy(a);
}

static RDecimal add(RDecimal a, const RDecimal *b)
{
    if (a.negative != b->negative) {
        if (magnitude(&a,b) >= 0)
            subtract_magnitude(&a,b);
        else {
            RDecimal larger = *b;
            subtract_magnitude(&larger,&a);
            a = larger;
        }
    } else {
        uint64_t carry = 0;
        int n = a.used > b->used ? a.used : b->used;
        for (int i = 0; i < n || carry != 0; i++) {
            uint64_t sum = (i < a.used ? a.word[i] : 0) +
                           (uint64_t)(i < b->used ? b->word[i] : 0) + carry;
            if (i >= a.used)
                a.used = i+1;
            a.word[i] = (uint32_t)(sum % R_BASE);
            carry = sum / R_BASE;
        }
    }
    tidy(&a);
    return a;
}

static RDecimal subtract(RDecimal a, RDecimal b)
{
    if (b.used != 0)
        b.negative = !b.negative;
    return add(a,&b);
}

static RDecimal from_double(double value)
{
    char text[32];
    p_number_spell(value,text);
    RDecimal out = {0};
    const char *s = text;
    if (*s == '-') {out.negative = true; s++;}
    bool fraction = false;
    while (*s != '\0' && *s != 'e') {
        if (*s == '.') {
            fraction = true;
        } else {
            multiply(&out,10);
            small_add(&out,(uint32_t)(*s-'0'));
            if (fraction)
                out.exponent--;
        }
        s++;
    }
    if (*s == 'e') {
        s++;
        bool negative = *s == '-';
        if (*s == '-' || *s == '+') s++;
        int exponent = 0;
        while (*s != '\0') exponent = exponent*10 + *s++-'0';
        out.exponent += negative ? -exponent : exponent;
    }
    // multiply() normalizes a zero coefficient while reading its digits.
    out.negative = value < 0 && out.used != 0;
    return out;
}

static void align(RDecimal *a, int exponent)
{
    static const uint32_t power[] = {1,10,100,1000,10000,100000,1000000,10000000,100000000};
    int places = a->exponent-exponent;
    int words = places/9;
    if (a->used != 0) {
        for (int i = a->used-1; i >= 0; i--)
            a->word[i+words] = a->word[i];
        for (int i = 0; i < words; i++)
            a->word[i] = 0;
        a->used += words;
        multiply(a,power[places%9]);
    }
    a->exponent = exponent;
}

static RDecimal range_remainder(RDecimal value, const RDecimal *divisor)
{
    RDecimal out = {.exponent=value.exponent};
    if (divisor->used == 1) {
        uint64_t rem = 0;
        for (int i = value.used-1; i >= 0; i--)
            rem = (rem*R_BASE + value.word[i]) % divisor->word[0];
        small_add(&out,(uint32_t)rem);
    } else {
        // Long division chooses each base-1e9 quotient digit by a bounded
        // binary search. Only the remainder is retained.
        for (int i = value.used-1; i >= 0; i--) {
            for (int j = out.used; j > 0; j--)
                out.word[j] = out.word[j-1];
            out.word[0] = value.word[i];
            out.used++;
            tidy(&out);
            if (magnitude(&out,divisor) < 0)
                continue;
            uint32_t low = 1, high = R_BASE-1, best = 0;
            while (low <= high) {
                uint32_t mid = low+(high-low)/2;
                RDecimal trial = *divisor;
                multiply(&trial,mid);
                if (magnitude(&trial,&out) <= 0) {best=mid;low=mid+1;}
                else high=mid-1;
            }
            RDecimal trial = *divisor;
            multiply(&trial,best);
            subtract_magnitude(&out,&trial);
        }
    }
    if (value.negative && out.used != 0) {
        RDecimal positive = *divisor;
        subtract_magnitude(&positive,&out);
        out = positive;
    }
    return out;
}

static bool to_double(const RDecimal *a, double *out)
{
    char text[R_WORDS*9+16];
    size_t at = 0;
    if (a->negative) text[at++]='-';
    if (a->used == 0) text[at++]='0';
    else {
        at += (size_t)os64_snprintf(text+at,sizeof(text)-at,"%u",a->word[a->used-1]);
        for (int i = a->used-2; i >= 0; i--)
            at += (size_t)os64_snprintf(text+at,sizeof(text)-at,"%09u",a->word[i]);
    }
    os64_snprintf(text+at,sizeof(text)-at,"e%d",a->exponent);
    // Grid candidates stay between finite bounds. The midpoint scale was
    // reserved before arithmetic, so this conversion has a finite result.
    return p_number_parse(text,true,out);
}

const char *p_range_value(PArena *arena, const os64_html_node_t *node,
                          const char *raw, size_t *len)
{
    double min = 0, max = 100, step = 1, base = 0, value = 0;
    bool min_spelled = p_number_parse(p_attr(node,"min"),false,&min);
    if (!min_spelled) min=0;
    if (!p_number_parse(p_attr(node,"max"),false,&max)) max=100;
    bool valid = p_number_parse(raw,true,&value);
    const char *step_text = p_attr(node,"step");
    bool any = step_text != NULL && os64_streq_nocase(step_text,"any");
    if (!p_number_parse(step_text,false,&step) || step <= 0) step=1;
    if (min_spelled) base=min;
    else if (!p_number_parse(p_attr(node,"value"),false,&base)) base=0;
    RDecimal low=from_double(min), high=from_double(max), stride=from_double(step),
             origin=from_double(base), chosen=from_double(value);
    int exponent=low.exponent;
    if (high.exponent < exponent) exponent=high.exponent;
    if (stride.exponent < exponent) exponent=stride.exponent;
    if (origin.exponent < exponent) exponent=origin.exponent;
    if (chosen.exponent < exponent) exponent=chosen.exponent;
    // One extra decimal place makes an odd midpoint sum exactly divisible.
    exponent--;
    align(&low,exponent);align(&high,exponent);align(&stride,exponent);
    align(&origin,exponent);align(&chosen,exponent);
    if (max < min) chosen=low;
    else if (!valid) {
        chosen=add(low,&high);
        uint64_t carry=0;
        for (int i=chosen.used-1;i>=0;i--) {
            uint64_t n=carry*R_BASE+chosen.word[i];
            chosen.word[i]=(uint32_t)(n/2);carry=n%2;
        }
        tidy(&chosen);
    }
    if (compare(&chosen,&low)<0) chosen=low;
    if (max>=min && compare(&chosen,&high)>0) chosen=high;
    if (!any && max>=min) {
        RDecimal rem=range_remainder(subtract(chosen,origin),&stride);
        RDecimal below=subtract(chosen,rem), above=add(below,&stride);
        bool below_ok=compare(&below,&low)>=0 && compare(&below,&high)<=0;
        bool above_ok=compare(&above,&low)>=0 && compare(&above,&high)<=0;
        multiply(&rem,2);
        if (below_ok && (!above_ok || magnitude(&rem,&stride)<0)) chosen=below;
        else if (above_ok) chosen=above;
    }
    double result=0;
    if (!to_double(&chosen,&result))
        return NULL;
    if (valid && result==value) {*len=os64_strlen(raw);return raw;}
    char text[32];*len=p_number_spell(result,text);
    return p_arena_copy(arena,text,*len);
}
