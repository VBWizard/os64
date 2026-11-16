#ifndef OS64_NATIVE_RETENTION_CASES_H
#define OS64_NATIVE_RETENTION_CASES_H
#include <html/retention.h>
#include <page/retention.h>
#include <os64/str.h>

/* Public API cases shared by the sanitized host and target consumer. */
static os64_html_document_t *native_retention_parse(const char *source)
{
    os64_html_parser_t *parser = os64_html_parser_new(NULL);
    if (!parser) return NULL;
    os64_html_parser_feed(parser, source, os64_strlen(source));
    return os64_html_parser_finish(parser);
}
static void native_retention_cases(void (*require)(bool, const char *), void (*purity)(bool))
{
    require(os64_html_retention_snapshot(NULL, SIZE_MAX).status == OS64_HTML_BAD_ARGUMENT &&
        os64_html_node_retention_snapshot(NULL, 0).owner.status == OS64_HTML_BAD_ARGUMENT &&
        !os64_page_retention_snapshot(NULL, SIZE_MAX).valid,
        "native retention: NULL is invalid, not an empty owner");
    os64_html_document_t *a = native_retention_parse("<body><input value=markup>"),
        *b = native_retention_parse("<body>");
    os64_html_group_t *group = os64_html_group_create(4u << 20);
    require(a && b && group && !os64_html_group_add(group, a, 3) &&
        !os64_html_group_add(group, b, 7), "native retention: grouped fixture");
    os64_html_retention_t ar = os64_html_retention_snapshot(a, 0),
        br = os64_html_retention_snapshot(b, 0);
    require(ar.status == OS64_HTML_OK && ar.grouped && ar.charged_to_group &&
        ar.readers.complete && ar.associated_ranges.complete && !ar.holds && !ar.snapshot_pins &&
        ar.unknown && ar.creation_realm == 3 && br.creation_realm == 7 && ar.arena_id != br.arena_id,
        "native retention: empty local census leaves graph coverage unknown");

    os64_page_state_t *sa = os64_page_state_create(a, 0), *sb = os64_page_state_create(b, 0);
    os64_page_state_group_t *states = os64_page_state_group_create(4u << 20);
    require(sa && sb && states && os64_page_state_group_add(states, sa) &&
        os64_page_state_group_add(states, sb), "native retention: page state group");
    os64_html_node_t *input = a->body->first_child;
    require(!os64_page_node_set_value(sa, input, "typed", 5), "native retention: dirty source value");
    os64_page_t *old = os64_page_build(a, "https://example.test/", NULL, sa);
    require(old && !os64_page_incomplete(old), "native retention: source model borrows values");
    const char *borrowed = NULL; size_t length = 0;
    require(!os64_page_node_value(sa, input, &borrowed, &length) && length == 5,
        "native retention: borrowed value before adoption");
    require(!os64_page_node_insert(sb, b->body, input, NULL) &&
        !os64_page_node_set_value(sb, input, "replacement", 11), "native retention: adopt and replace");
    os64_html_hold(b, input);
    os64_html_node_retention_t node = os64_html_node_retention_snapshot(input, SIZE_MAX);
    require(node.owner.arena_id == ar.arena_id && node.logical_document_arena_id == br.arena_id &&
        node.creation_realm == 3 && node.node_nonparser_refs && node.owner.holds &&
        !os64_html_retention_snapshot(b, SIZE_MAX).snapshot_pins,
        "native retention: adopted node reports physical owner and original creation slot");
    os64_page_retention_t page = os64_page_retention_snapshot(sa, 0);
    require(page.valid && page.grouped && !page.private_owner && !page.models_seen && !page.models_complete &&
        (page.unknown & OS64_PAGE_RETENTION_UNKNOWN_MODELS),
        "native retention: zero budget cannot certify no models");
    page = os64_page_retention_snapshot(sa, 1);
    require(page.models_seen == 1 && page.models_complete &&
        !(page.unknown & OS64_PAGE_RETENTION_UNKNOWN_MODELS) &&
        (page.unknown & OS64_PAGE_RETENTION_UNKNOWN_MODEL_REFERENCES),
        "native retention: complete model list still leaves references unknown");
    require(os64_page_retain(old), "native retention: extra reference to one model");
    os64_page_free(old);
    require(os64_page_retention_snapshot(sa, SIZE_MAX).models_seen == 1,
        "native retention: nonfinal reference release keeps one model");
    os64_page_t *second = os64_page_build(a, "https://example.test/", NULL, sa);
    os64_page_t *foreign = os64_page_build(b, "https://example.test/", NULL, sb);
    require(second && foreign && !os64_page_retention_snapshot(sa, 1).models_complete &&
        os64_page_retention_snapshot(sa, 1).models_seen == 1 &&
        os64_page_retention_snapshot(sa, SIZE_MAX).models_seen == 2,
        "native retention: model list truncation is explicit");
    size_t retained = os64_page_state_bytes(sa);
    os64_page_free(old);
    require(os64_page_retention_snapshot(sa, SIZE_MAX).physical_bytes == retained &&
        !os64_memcmp(borrowed, "typed", 6), "native retention: another model preserves retired source value");
    os64_page_free(second);
    page = os64_page_retention_snapshot(sa, 0);
    require(page.models_complete && !page.models_seen && page.physical_bytes == retained &&
        (page.unknown & OS64_PAGE_RETENTION_UNKNOWN_GROUP_BORROWS) && !os64_memcmp(borrowed, "typed", 6),
        "native retention: zero local models still permits foreign borrowed source bytes");
    os64_page_free(foreign);
    require(os64_page_retention_snapshot(sa, 0).models_complete &&
        os64_page_retention_snapshot(sa, 0).physical_bytes < retained,
        "native retention: final model release reclaims physical source charge");

    os64_html_reader_t *readers[65]; os64_html_range_t *ranges[65];
    for (size_t i = 0; i < 65; i++) {
        readers[i] = os64_html_reader_new(a, os64_html_version(a));
        ranges[i] = os64_html_range_new(a);
        require(readers[i] && ranges[i], "native retention: reader and range borrowers");
    }
    ar = os64_html_retention_snapshot(a, 0);
    require(!ar.readers.seen && !ar.readers.complete && !ar.associated_ranges.complete &&
        (ar.unknown & OS64_HTML_RETENTION_UNKNOWN_READERS) &&
        (ar.unknown & OS64_HTML_RETENTION_UNKNOWN_RANGES),
        "native retention: zero prefix leaves reader and range coverage unknown");
    ar = os64_html_retention_snapshot(a, SIZE_MAX);
    require(ar.readers.seen == 64 && !ar.readers.complete && ar.associated_ranges.seen == 64 &&
        !ar.associated_ranges.complete, "native retention: huge scan budget clamps each list at 64");
    os64_html_reader_free(readers[64]); os64_html_range_free(ranges[64]);
    ar = os64_html_retention_snapshot(a, SIZE_MAX);
    require(ar.readers.seen == 64 && ar.readers.complete && ar.associated_ranges.seen == 64 &&
        ar.associated_ranges.complete, "native retention: exact boundary is complete");
    ar = os64_html_retention_snapshot(a, 1);
    require(ar.readers.seen == 1 && !ar.readers.complete && ar.associated_ranges.seen == 1 &&
        !ar.associated_ranges.complete, "native retention: caller budget bounds each prefix");
    for (size_t i = 0; i < 64; i++) {
        os64_html_reader_free(readers[i]); os64_html_range_free(ranges[i]);
    }

    os64_html_node_t *detached = os64_html_create_text(a, "range", 5, NULL);
    os64_html_range_t *moved_range = os64_html_range_new(a);
    require(detached && moved_range && !os64_html_range_set(moved_range, 0, detached, 0) &&
        !os64_html_range_set(moved_range, 1, detached, 3) && !os64_html_adopt(b, b->document, detached),
        "native retention: live range follows detached node adoption");
    require(!os64_html_retention_snapshot(a, SIZE_MAX).associated_ranges.seen &&
        os64_html_retention_snapshot(b, SIZE_MAX).associated_ranges.seen == 1 &&
        os64_html_node_retention_snapshot(detached, 0).owner.arena_id == ar.arena_id,
        "native retention: range association migrates independently of physical owner");
    os64_html_range_free(moved_range);

    os64_html_node_t *text = os64_html_create_text(b, "before", 6, NULL);
    require(text && !os64_html_insert(b, b->body, text, NULL), "native retention: text fixture");
    os64_html_pin_t pin = os64_html_pin(a);
    const char *old_text = text->text;
    require(pin && !os64_html_set_text(b, text, "after", 5), "native retention: sibling pin retains text");
    ar = os64_html_retention_snapshot(a, SIZE_MAX); br = os64_html_retention_snapshot(b, SIZE_MAX);
    require(ar.snapshot_pins == 1 && !br.snapshot_pins && br.retired_block_bytes &&
        (br.unknown & OS64_HTML_RETENTION_UNKNOWN_GROUP_BORROWS) && !os64_memcmp(old_text, "before", 7),
        "native retention: zero local pins does not mean no retained storage");
    uint64_t av = os64_html_version(a), bv = os64_html_version(b), sv = os64_page_state_version(sa);
    size_t html_bytes = os64_html_group_bytes(group), page_bytes = os64_page_state_group_bytes(states);
    if (purity) purity(true);
    for (size_t i = 0; i < 100; i++) {
        os64_html_retention_t probe = os64_html_retention_snapshot(b, SIZE_MAX);
        os64_page_retention_t state = os64_page_retention_snapshot(sa, SIZE_MAX);
        os64_html_node_retention_t moved = os64_html_node_retention_snapshot(input, SIZE_MAX);
        require(probe.retired_block_bytes == br.retired_block_bytes && probe.arena_bytes == br.arena_bytes &&
            probe.peak_arena_bytes == br.peak_arena_bytes && state.physical_bytes == os64_page_state_bytes(sa) &&
            moved.owner.arena_id == node.owner.arena_id, "native retention: queries preserve physical ledgers");
    }
    if (purity) purity(false);
    require(av == os64_html_version(a) && bv == os64_html_version(b) && sv == os64_page_state_version(sa) &&
        html_bytes == os64_html_group_bytes(group) && page_bytes == os64_page_state_group_bytes(states),
        "native retention: queries do not mutate versions or group charges");
    os64_html_unpin(a, pin);
    require(!os64_html_retention_snapshot(b, SIZE_MAX).retired_block_bytes,
        "native retention: explicit sibling unpin releases retained blocks");
    os64_html_release(b, input);
    os64_page_state_group_free(states); os64_html_group_free(group);

    os64_html_parser_t *parser = os64_html_parser_new(NULL);
    os64_html_document_t *doc = os64_html_parser_document(parser);
    require(parser && os64_html_retention_snapshot(doc, 0).parser_active,
        "native retention: parser borrower remains visible without collection");
    doc = os64_html_parser_finish(parser);
    require(!os64_html_retention_snapshot(doc, 0).parser_active, "native retention: parser finish clears borrower");
    os64_page_t *private_model = os64_page_build(doc, "https://example.test/", NULL, NULL);
    require(private_model && os64_page_retention_snapshot(os64_page_shared_state(private_model), SIZE_MAX).private_owner,
        "native retention: private model state is descriptive only");
    os64_page_free(private_model); os64_html_document_free(doc);
}
#endif
