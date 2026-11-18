#ifndef JS_OWNER_TICKET_CASES_H
#define JS_OWNER_TICKET_CASES_H
#include "../runtime/owner_tickets.h"

typedef struct {
    void (*check)(bool, const char *);
    void *(*alloc)(size_t);
    void (*release)(void *);
} OwnerTestHost;
typedef struct { JSOwnerTickets table; void *storage; size_t bytes; uint64_t incarnation; } OwnerFixture;
static bool owner_id_equal(JSOwnerId a, JSOwnerId b)
{ return a.incarnation == b.incarnation && a.serial == b.serial; }
static bool owner_ticket_equal(JSOwnerTicket a, JSOwnerTicket b)
{ return owner_id_equal(a.owner,b.owner) && a.serial == b.serial; }
static bool owner_loan_equal(JSOwnerLoan a, JSOwnerLoan b)
{ return owner_id_equal(a.owner,b.owner) && a.serial == b.serial; }
static void owner_fixture_open(OwnerFixture *f, const OwnerTestHost *h, JSOwnerLimits limits, uint64_t incarnation)
{
    h->check(js_owner_tickets_size(limits,&f->bytes)==JS_OWNER_OK,"owners: checked storage size");
    f->storage=h->alloc(f->bytes); f->incarnation=incarnation;
    h->check(f->storage!=NULL,"owners: caller allocates exact storage");
    h->check(js_owner_tickets_init(&f->table,incarnation,limits,f->storage,f->bytes)==JS_OWNER_OK,
        "owners: initialize caller-owned table");
}
static void owner_fixture_close(OwnerFixture *f, const OwnerTestHost *h)
{
    JSOwnerSummary s;
    h->check(js_owner_summary(&f->table,f->incarnation,&s)==JS_OWNER_OK && !s.tickets && !s.loans,
        "owners: external users unwind before table storage ends");
    h->release(f->storage);
}
static void owner_ticket_transitions(const OwnerTestHost *h)
{
    OwnerFixture f={0}; owner_fixture_open(&f,h,(JSOwnerLimits){2,3,2},11);
    JSOwnerTicket a,b,d,p,alias,unchanged={{900,901},902};
    JSOwnerLoan first,second,loan_sentinel={{700,701},702}; JSOwnerId key={800,801};
    h->check(js_owner_ticket_create(&f.table,11,&a)==JS_OWNER_OK &&
        js_owner_ticket_create(&f.table,11,&b)==JS_OWNER_OK && !owner_id_equal(a.owner,b.owner),
        "owners: distinct metadata owners");
    alias=a;
    h->check(js_owner_ticket_duplicate(&f.table,11,a,&d)==JS_OWNER_OK && d.serial!=a.serial &&
        owner_id_equal(d.owner,a.owner),"owners: explicit duplicate is an independent holder");
    h->check(js_owner_loan_begin(&f.table,11,a,&first)==JS_OWNER_OK &&
        js_owner_loan_begin(&f.table,11,a,&second)==JS_OWNER_OK,
        "owners: admit independent loans");
    JSOwnerTicket loan_as_ticket={first.owner,first.serial};
    JSOwnerLoan ticket_as_loan={d.owner,d.serial};
    h->check(js_owner_ticket_resolve(&f.table,11,loan_as_ticket,&key)==JS_OWNER_STALE &&
        js_owner_loan_owner(&f.table,11,ticket_as_loan,&key)==JS_OWNER_STALE,
        "owners: active cross-kind tokens cannot resolve in the other pool");
    uint64_t last=f.table._last_token;
    p=unchanged;
    h->check(js_owner_loan_promote(&f.table,11,first,&p)==JS_OWNER_CAPACITY &&
        owner_ticket_equal(p,unchanged) && f.table._last_token==last &&
        js_owner_loan_owner(&f.table,11,first,&key)==JS_OWNER_OK && owner_id_equal(key,a.owner),
        "owners: full-pool promotion preserves loan, output and issuer");
    JSOwnerLoan refused=loan_sentinel;
    h->check(js_owner_loan_begin(&f.table,11,a,&refused)==JS_OWNER_CAPACITY &&
        owner_loan_equal(refused,loan_sentinel) && f.table._last_token==last,
        "owners: loan capacity refuses without consuming identity");
    JSOwnerCounts counts;
    h->check(js_owner_counts(&f.table,11,a.owner,&counts)==JS_OWNER_OK && counts.tickets==2 && counts.loans==2,
        "owners: copying a ticket adds no holder");
    h->check(js_owner_ticket_close(&f.table,11,alias)==JS_OWNER_OK,
        "owners: close a copied alias");
    key=(JSOwnerId){800,801};
    h->check(js_owner_ticket_resolve(&f.table,11,a,&key)==JS_OWNER_STALE && key.serial==801,
        "owners: closing an alias invalidates original without writing output");
    p=unchanged;
    h->check(js_owner_ticket_duplicate(&f.table,11,a,&p)==JS_OWNER_STALE && owner_ticket_equal(p,unchanged),
        "owners: closed ticket cannot duplicate");
    h->check(js_owner_ticket_resolve(&f.table,11,d,&key)==JS_OWNER_OK,
        "owners: explicit duplicate survives alias close");
    h->check(js_owner_loan_promote(&f.table,11,first,&p)==JS_OWNER_OK && p.serial>last &&
        owner_id_equal(p.owner,a.owner),"owners: reentrant close then loan promotion");
    h->check(js_owner_ticket_close(&f.table,11,a)==JS_OWNER_STALE &&
        js_owner_ticket_resolve(&f.table,11,p,&key)==JS_OWNER_OK,
        "owners: stale close cannot affect reused holder cell");
    JSOwnerTicket wrong_kind={first.owner,first.serial}; JSOwnerLoan other_kind={p.owner,p.serial};
    h->check(js_owner_ticket_resolve(&f.table,11,wrong_kind,&key)==JS_OWNER_STALE &&
        js_owner_loan_owner(&f.table,11,other_kind,&key)==JS_OWNER_STALE,
        "owners: shared issuance prevents cross-kind token confusion");
    JSOwnerTicket wrong_owner={b.owner,p.serial};
    h->check(js_owner_ticket_resolve(&f.table,11,wrong_owner,&key)==JS_OWNER_STALE,
        "owners: token must match its original owner");
    h->check(js_owner_ticket_close(&f.table,11,d)==JS_OWNER_OK &&
        js_owner_ticket_close(&f.table,11,p)==JS_OWNER_OK &&
        js_owner_counts(&f.table,11,a.owner,&counts)==JS_OWNER_OK && !counts.tickets && counts.loans==2,
        "owners: admitted loans survive the last ticket close");
    h->check(js_owner_loan_owner(&f.table,11,first,&key)==JS_OWNER_OK &&
        js_owner_loan_end(&f.table,11,first)==JS_OWNER_OK &&
        js_owner_loan_end(&f.table,11,first)==JS_OWNER_STALE &&
        js_owner_loan_owner(&f.table,11,second,&key)==JS_OWNER_OK,
        "owners: ending one loan does not end its independent sibling");
    p=unchanged;
    h->check(js_owner_loan_promote(&f.table,11,first,&p)==JS_OWNER_STALE && owner_ticket_equal(p,unchanged),
        "owners: ended loan cannot promote");
    h->check(js_owner_loan_end(&f.table,11,second)==JS_OWNER_OK &&
        js_owner_ticket_close(&f.table,11,b)==JS_OWNER_OK,"owners: explicit final metadata closes");
    JSOwnerSummary summary;
    p=unchanged;
    h->check(js_owner_ticket_create(&f.table,11,&p)==JS_OWNER_CAPACITY && owner_ticket_equal(p,unchanged) &&
        js_owner_summary(&f.table,11,&summary)==JS_OWNER_OK && summary.owners==2 && !summary.tickets && !summary.loans,
        "owners: closed holders do not recycle owner rows");
    owner_fixture_close(&f,h);
}
static void owner_ticket_capacity_and_exhaustion(const OwnerTestHost *h)
{
    OwnerFixture f={0}; owner_fixture_open(&f,h,(JSOwnerLimits){3,1,1},21);
    JSOwnerTicket t,sentinel={{91,92},93},out=sentinel; JSOwnerLoan loan;
    h->check(js_owner_ticket_create(&f.table,21,&t)==JS_OWNER_OK,"owners: capacity fixture");
    h->check(js_owner_ticket_create(&f.table,21,&out)==JS_OWNER_CAPACITY &&
        owner_ticket_equal(out,sentinel) && f.table._owners_used==1 && f.table._last_owner==1,
        "owners: holder refusal cannot partially create an owner");
    h->check(js_owner_loan_begin(&f.table,21,t,&loan)==JS_OWNER_OK &&
        js_owner_ticket_close(&f.table,21,t)==JS_OWNER_OK &&
        js_owner_loan_promote(&f.table,21,loan,&out)==JS_OWNER_OK &&
        js_owner_ticket_close(&f.table,21,out)==JS_OWNER_OK &&
        js_owner_loan_end(&f.table,21,loan)==JS_OWNER_OK,"owners: capacity recovery through live loan");
    owner_fixture_close(&f,h);

    f=(OwnerFixture){0}; owner_fixture_open(&f,h,(JSOwnerLimits){3,3,2},22);
    /* Fault injection advances private issuers; it does not reset identities. */
    f.table._last_owner=UINT64_MAX-1;
    h->check(js_owner_ticket_create(&f.table,22,&t)==JS_OWNER_OK && t.owner.serial==UINT64_MAX,
        "owners: final representable owner identity is valid");
    out=sentinel;
    uint64_t token=f.table._last_token;
    h->check(js_owner_ticket_create(&f.table,22,&out)==JS_OWNER_EXHAUSTED &&
        owner_ticket_equal(out,sentinel) && f.table._owners_used==1 && f.table._last_token==token,
        "owners: exhausted owner issuer refuses without wrap or partial holder");
    h->check(js_owner_ticket_close(&f.table,22,t)==JS_OWNER_OK,"owners: close after owner exhaustion");
    owner_fixture_close(&f,h);

    f=(OwnerFixture){0}; owner_fixture_open(&f,h,(JSOwnerLimits){3,3,2},23);
    f.table._last_token=UINT64_MAX-1;
    h->check(js_owner_ticket_create(&f.table,23,&t)==JS_OWNER_OK && t.serial==UINT64_MAX,
        "owners: final representable shared token identity is valid");
    out=sentinel; JSOwnerLoan loan_out={{81,82},83},loan_before=loan_out;
    h->check(js_owner_ticket_duplicate(&f.table,23,t,&out)==JS_OWNER_EXHAUSTED && owner_ticket_equal(out,sentinel) &&
        js_owner_loan_begin(&f.table,23,t,&loan_out)==JS_OWNER_EXHAUSTED && owner_loan_equal(loan_out,loan_before),
        "owners: shared ticket/loan issuer refuses without wrap");
    h->check(js_owner_ticket_close(&f.table,23,t)==JS_OWNER_OK &&
        js_owner_ticket_create(&f.table,23,&out)==JS_OWNER_EXHAUSTED &&
        f.table._owners_used==1 && f.table._last_owner==1,
        "owners: exhausted token issuer cannot consume an owner row");
    owner_fixture_close(&f,h);

    f=(OwnerFixture){0}; owner_fixture_open(&f,h,(JSOwnerLimits){2,2,1},24);
    h->check(js_owner_ticket_create(&f.table,24,&t)==JS_OWNER_OK &&
        js_owner_loan_begin(&f.table,24,t,&loan)==JS_OWNER_OK &&
        js_owner_ticket_close(&f.table,24,t)==JS_OWNER_OK,"owners: promotion exhaustion fixture");
    f.table._last_token=UINT64_MAX; out=sentinel;
    JSOwnerId id;
    h->check(js_owner_loan_promote(&f.table,24,loan,&out)==JS_OWNER_EXHAUSTED && owner_ticket_equal(out,sentinel) &&
        js_owner_loan_owner(&f.table,24,loan,&id)==JS_OWNER_OK &&
        js_owner_loan_end(&f.table,24,loan)==JS_OWNER_OK,"owners: issuer refusal preserves the admitted loan");
    owner_fixture_close(&f,h);
}
static void owner_ticket_storage_and_incarnation(const OwnerTestHost *h)
{
    JSOwnerLimits limits={2,3,2}; size_t bytes=0,sentinel=777;
    h->check(js_owner_tickets_size(limits,&bytes)==JS_OWNER_OK && bytes>0,"owners: exact arena requirement");
    h->check(js_owner_tickets_size((JSOwnerLimits){0,1,1},&sentinel)==JS_OWNER_INVALID && sentinel==777 &&
        js_owner_tickets_size((JSOwnerLimits){OS64_JS_MAX_REALMS+1,1,1},&sentinel)==JS_OWNER_INVALID &&
        js_owner_tickets_size((JSOwnerLimits){1,SIZE_MAX,1},&sentinel)==JS_OWNER_INVALID &&
        js_owner_tickets_size((JSOwnerLimits){1,1,SIZE_MAX},&sentinel)==JS_OWNER_INVALID && sentinel==777,
        "owners: capacity and size overflow refuse without writing output");
    unsigned char *storage=h->alloc(bytes+_Alignof(max_align_t));
    h->check(storage!=NULL,"owners: invalid-storage fixture");
    for(size_t i=0;i<bytes+_Alignof(max_align_t);i++)storage[i]=0xa5;
    JSOwnerTickets table={0};
    h->check(js_owner_tickets_init(&table,31,limits,storage,bytes-1)==JS_OWNER_INVALID &&
        js_owner_tickets_init(&table,31,limits,storage+1,bytes)==JS_OWNER_INVALID &&
        js_owner_tickets_init(&table,31,limits,storage,SIZE_MAX)==JS_OWNER_INVALID &&
        js_owner_tickets_init(&table,0,limits,storage,bytes)==JS_OWNER_INVALID &&
        js_owner_tickets_init(&table,31,(JSOwnerLimits){1,1,1},&table,sizeof table)==JS_OWNER_INVALID,
        "owners: short, unaligned, wrapping, zero-incarnation and overlapping storage refuse");
    bool pristine=table._incarnation==0;
    for(size_t i=0;i<bytes+_Alignof(max_align_t);i++)pristine&=storage[i]==0xa5;
    h->check(pristine,"owners: invalid init does not modify table or arena");
    h->check(js_owner_tickets_init(&table,31,limits,storage,bytes)==JS_OWNER_OK,"owners: valid exact arena");
    JSOwnerTicket old;
    h->check(js_owner_ticket_create(&table,31,&old)==JS_OWNER_OK &&
        js_owner_tickets_init(&table,32,limits,storage,bytes)==JS_OWNER_INVALID,
        "owners: init cannot reset a live table");
    JSOwnerId id;
    h->check(js_owner_ticket_resolve(&table,31,old,&id)==JS_OWNER_OK &&
        js_owner_ticket_close(&table,31,old)==JS_OWNER_OK,"owners: rejected reset preserves old ticket");
    for(size_t i=bytes;i<bytes+_Alignof(max_align_t);i++)h->check(storage[i]==0xa5,"owners: init respects supplied arena extent");
    h->release(storage);

    /* Simulate a new table lifetime at the same address, with supplied fresh
     * test IDs. This checks numeric binding, not a production identity issuer. */
    table=(JSOwnerTickets){0}; storage=h->alloc(bytes);
    h->check(storage && js_owner_tickets_init(&table,32,limits,storage,bytes)==JS_OWNER_OK,
        "owners: supplied new live incarnation");
    JSOwnerTicket current;
    h->check(js_owner_ticket_create(&table,32,&current)==JS_OWNER_OK &&
        current.serial==old.serial && current.owner.serial==old.owner.serial,
        "owners: same local numbers in a distinct test incarnation");
    id=(JSOwnerId){900,901};
    h->check(js_owner_ticket_resolve(&table,32,old,&id)==JS_OWNER_FOREIGN && id.serial==901 &&
        js_owner_ticket_resolve(&table,31,old,&id)==JS_OWNER_FOREIGN &&
        js_owner_ticket_resolve(&table,31,current,&id)==JS_OWNER_FOREIGN,
        "owners: stale token and wrong live incarnation cannot bind a new table");
    JSOwnerCounts counts={70,71}; JSOwnerSummary summary;
    h->check(js_owner_counts(&table,32,old.owner,&counts)==JS_OWNER_FOREIGN && counts.tickets==70 &&
        js_owner_note_unknown_escape(&table,31)==JS_OWNER_FOREIGN &&
        js_owner_summary(&table,32,&summary)==JS_OWNER_OK && !summary.unknown_escape,
        "owners: foreign identity cannot mutate metadata or output");
    h->check(js_owner_ticket_resolve(&table,32,(JSOwnerTicket){0},&id)==JS_OWNER_INVALID &&
        js_owner_ticket_resolve(&table,32,current,NULL)==JS_OWNER_INVALID &&
        js_owner_ticket_create(NULL,32,&current)==JS_OWNER_INVALID &&
        js_owner_ticket_close(&table,32,current)==JS_OWNER_OK,"owners: invalid inputs preserve valid holders");
    h->release(storage);
}
static void owner_ticket_churn(const OwnerTestHost *h)
{
    OwnerFixture f={0}; owner_fixture_open(&f,h,(JSOwnerLimits){1,2,1},41);
    JSOwnerTicket anchor;
    h->check(js_owner_ticket_create(&f.table,41,&anchor)==JS_OWNER_OK,"owners: churn anchor");
    uint64_t last=anchor.serial;
    for(size_t i=0;i<4096;i++) {
        JSOwnerTicket t,p; JSOwnerLoan loan; JSOwnerId id;
        h->check(js_owner_ticket_duplicate(&f.table,41,anchor,&t)==JS_OWNER_OK && t.serial>last,
            "owners: repeated holder reuse issues fresh identities");
        h->check(js_owner_ticket_close(&f.table,41,t)==JS_OWNER_OK &&
            js_owner_loan_begin(&f.table,41,anchor,&loan)==JS_OWNER_OK && loan.serial>t.serial &&
            js_owner_loan_promote(&f.table,41,loan,&p)==JS_OWNER_OK && p.serial>loan.serial,
            "owners: shared issuance separates reused ticket and loan cells");
        h->check(js_owner_ticket_resolve(&f.table,41,t,&id)==JS_OWNER_STALE &&
            js_owner_ticket_close(&f.table,41,t)==JS_OWNER_STALE &&
            js_owner_ticket_resolve(&f.table,41,p,&id)==JS_OWNER_OK,
            "owners: stale history cannot affect a later holder");
        h->check(js_owner_ticket_close(&f.table,41,p)==JS_OWNER_OK &&
            js_owner_loan_end(&f.table,41,loan)==JS_OWNER_OK,"owners: churn releases metadata holders");
        JSOwnerSummary s;
        h->check(js_owner_summary(&f.table,41,&s)==JS_OWNER_OK && s.owners==1 && s.tickets==1 && !s.loans,
            "owners: bounded cells do not accumulate token history");
        last=p.serial;
    }
    h->check(js_owner_note_unknown_escape(&f.table,41)==JS_OWNER_OK &&
        js_owner_ticket_close(&f.table,41,anchor)==JS_OWNER_OK,"owners: mark unknown retention and close");
    JSOwnerSummary s;
    h->check(js_owner_summary(&f.table,41,&s)==JS_OWNER_OK && s.unknown_escape && !s.tickets && !s.loans && s.owners==1,
        "owners: unknown escape survives zero holder counts");
    owner_fixture_close(&f,h);
}
/* An operation-history model derives expected per-owner counts from live
 * returned IDs, independently of the implementation's reusable cell layout. */
static void owner_ticket_model(const OwnerTestHost *h)
{
    enum { STEPS=512, HISTORY=STEPS+3 };
    typedef struct { JSOwnerTicket value; size_t owner; bool live; } ModelTicket;
    typedef struct { JSOwnerLoan value; size_t owner; bool live; } ModelLoan;
    ModelTicket tickets[HISTORY]; ModelLoan loans[HISTORY];
    size_t nt=0,nl=0,used_t=0,used_l=0;
    OwnerFixture f={0}; owner_fixture_open(&f,h,(JSOwnerLimits){3,5,3},51);
    JSOwnerId owners[3]; uint64_t last=0; bool unknown=false;
    for(size_t i=0;i<3;i++) {
        JSOwnerTicket t;
        h->check(js_owner_ticket_create(&f.table,51,&t)==JS_OWNER_OK,"owners model: anchor");
        owners[i]=t.owner; tickets[nt++]=(ModelTicket){t,i,true}; used_t++; last=t.serial;
    }
    uint32_t random=UINT32_C(0x6400f15c);
    for(size_t step=0;step<STEPS;step++) {
        random=random*UINT32_C(1664525)+UINT32_C(1013904223);
        unsigned op=random%7;
        size_t ti=(random&8)?(random>>8)%3:(random>>8)%nt;
        size_t li=nl?(random>>12)%nl:0;
        JSOwnerStatus expected;
        JSOwnerTicket out={{81,82},83},before=out;
        JSOwnerLoan lout={{71,72},73},lbefore=lout;
        if(op==0) {
            expected=!tickets[ti].live?JS_OWNER_STALE:used_t==5?JS_OWNER_CAPACITY:JS_OWNER_OK;
            h->check(js_owner_ticket_duplicate(&f.table,51,tickets[ti].value,&out)==expected,
                "owners model: duplicate result");
            if(expected==JS_OWNER_OK) {
                h->check(out.serial>last && owner_id_equal(out.owner,owners[tickets[ti].owner]),
                    "owners model: duplicate identity");
                last=out.serial; tickets[nt++]=(ModelTicket){out,tickets[ti].owner,true}; used_t++;
            } else h->check(owner_ticket_equal(out,before),"owners model: refused duplicate output");
        } else if(op==1) {
            if(nt>3) {
                ti=3+(random>>8)%(nt-3);
                expected=tickets[ti].live?JS_OWNER_OK:JS_OWNER_STALE;
                h->check(js_owner_ticket_close(&f.table,51,tickets[ti].value)==expected,
                    "owners model: close result");
                if(expected==JS_OWNER_OK) { tickets[ti].live=false; used_t--; }
            }
        } else if(op==2) {
            expected=!tickets[ti].live?JS_OWNER_STALE:used_l==3?JS_OWNER_CAPACITY:JS_OWNER_OK;
            h->check(js_owner_loan_begin(&f.table,51,tickets[ti].value,&lout)==expected,
                "owners model: loan admission result");
            if(expected==JS_OWNER_OK) {
                h->check(lout.serial>last && owner_id_equal(lout.owner,owners[tickets[ti].owner]),
                    "owners model: shared issuance identity");
                last=lout.serial; loans[nl++]=(ModelLoan){lout,tickets[ti].owner,true}; used_l++;
            } else h->check(owner_loan_equal(lout,lbefore),"owners model: refused loan output");
        } else if(op==3 && nl) {
            expected=loans[li].live?JS_OWNER_OK:JS_OWNER_STALE;
            h->check(js_owner_loan_end(&f.table,51,loans[li].value)==expected,"owners model: loan end result");
            if(expected==JS_OWNER_OK) { loans[li].live=false; used_l--; }
        } else if(op==4 && nl) {
            expected=!loans[li].live?JS_OWNER_STALE:used_t==5?JS_OWNER_CAPACITY:JS_OWNER_OK;
            h->check(js_owner_loan_promote(&f.table,51,loans[li].value,&out)==expected,
                "owners model: promotion result");
            if(expected==JS_OWNER_OK) {
                h->check(out.serial>last && owner_id_equal(out.owner,owners[loans[li].owner]),
                    "owners model: promotion identity");
                last=out.serial; tickets[nt++]=(ModelTicket){out,loans[li].owner,true}; used_t++;
            } else h->check(owner_ticket_equal(out,before),"owners model: refused promotion output");
        } else if(op==5) {
            JSOwnerId out_id={91,92};
            expected=tickets[ti].live?JS_OWNER_OK:JS_OWNER_STALE;
            h->check(js_owner_ticket_resolve(&f.table,51,tickets[ti].value,&out_id)==expected &&
                (expected==JS_OWNER_OK?owner_id_equal(out_id,owners[tickets[ti].owner]):out_id.serial==92),
                "owners model: identity-only resolve");
        } else if(op==6) {
            h->check(js_owner_note_unknown_escape(&f.table,51)==JS_OWNER_OK,"owners model: unknown retention");
            unknown=true;
        }
        size_t expected_t[3]={0},expected_l[3]={0};
        for(size_t i=0;i<nt;i++)if(tickets[i].live)expected_t[tickets[i].owner]++;
        for(size_t i=0;i<nl;i++)if(loans[i].live)expected_l[loans[i].owner]++;
        for(size_t i=0;i<3;i++) {
            JSOwnerCounts c;
            h->check(js_owner_counts(&f.table,51,owners[i],&c)==JS_OWNER_OK &&
                c.tickets==expected_t[i] && c.loans==expected_l[i],"owners model: per-owner balance");
        }
        JSOwnerSummary summary;
        h->check(js_owner_summary(&f.table,51,&summary)==JS_OWNER_OK && summary.owners==3 &&
            summary.tickets==used_t && summary.loans==used_l && summary.unknown_escape==unknown,
            "owners model: table balance and sticky unknowns");
    }
    for(size_t i=0;i<nt;i++)if(tickets[i].live)
        h->check(js_owner_ticket_close(&f.table,51,tickets[i].value)==JS_OWNER_OK,"owners model: final ticket close");
    for(size_t i=0;i<nl;i++)if(loans[i].live)
        h->check(js_owner_loan_end(&f.table,51,loans[i].value)==JS_OWNER_OK,"owners model: final loan end");
    owner_fixture_close(&f,h);
}
static void owner_ticket_cases(const OwnerTestHost *h)
{
    owner_ticket_transitions(h);
    owner_ticket_capacity_and_exhaustion(h);
    owner_ticket_storage_and_incarnation(h);
    owner_ticket_churn(h);
    owner_ticket_model(h);
    OwnerFixture f={0}; owner_fixture_open(&f,h,(JSOwnerLimits){3,1,1},61);
    JSOwnerId primary,child,unused={91,92}; JSOwnerTicket ticket,sentinel={{81,82},83},out=sentinel;
    h->check(js_owner_reserve(&f.table,61,&primary)==JS_OWNER_OK && f.table._last_token==0,
        "owners: row-only reservation consumes no holder identity");
    h->check(js_owner_ticket_open(&f.table,61,primary,&ticket)==JS_OWNER_OK &&
        js_owner_reserve(&f.table,61,&child)==JS_OWNER_OK && !owner_id_equal(primary,child),
        "owners: native owner reservation is independent of a full holder pool");
    h->check(js_owner_ticket_open(&f.table,61,child,&out)==JS_OWNER_CAPACITY && owner_ticket_equal(out,sentinel),
        "owners: opening known owner refuses atomically when holders are full");
    h->check(js_owner_ticket_close(&f.table,61,ticket)==JS_OWNER_OK &&
        js_owner_ticket_open(&f.table,61,child,&out)==JS_OWNER_OK && owner_id_equal(out.owner,child),
        "owners: known owner opens a fresh independent holder after cell reuse");
    h->check(js_owner_ticket_close(&f.table,61,out)==JS_OWNER_OK,
        "owners: explicitly close known-owner holder");
    out=sentinel;
    h->check(js_owner_ticket_open(&f.table,61,unused,&out)==JS_OWNER_FOREIGN && owner_ticket_equal(out,sentinel) &&
        js_owner_ticket_open(&f.table,61,(JSOwnerId){61,999},&out)==JS_OWNER_STALE &&
        js_owner_reserve(&f.table,61,NULL)==JS_OWNER_INVALID,
        "owners: row-only and known-owner refusals preserve outputs");
    uint64_t last_token=f.table._last_token;
    f.table._last_owner=UINT64_MAX;
    h->check(js_owner_reserve(&f.table,61,&unused)==JS_OWNER_EXHAUSTED && unused.serial==92 &&
        f.table._owners_used==2 && f.table._last_token==last_token,
        "owners: row-only issuer exhaustion leaves existing owners intact");
    owner_fixture_close(&f,h);
}
#endif
