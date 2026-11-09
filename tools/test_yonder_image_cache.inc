/* I1: the production picture worker and native fetch engine, real cache
 * on the fixture disk, fake HTTP with recorded requests, and real PNG decode. */
static yonder_picture_t *image_cache_run(way_cache_t *cache, const char *url,
    unsigned mode, const way_identity_t *identity)
{
    yonder_picture_job_t *job = sizes_job(1, 1);
    os64_strcopy(job->url, sizeof(job->url), url);
    os64_strcopy(job->hooks.referrer, sizeof(job->hooks.referrer), "https://a.test/page");
    job->canvas_request = mode;
    if(!mode){job->recoverable=true;job->cover_w=job->cover_h=0;}
    job->svg_enabled = true;
    job->hooks.cache = cache;
    job->hooks.identity = identity;
    void *out = NULL;
    yonder_picture_run(job, NULL, NULL, &out);
    yonder_picture_release(job, NULL);
    return out;
}

static void image_cache_expect(way_cache_t *cache, const char *url, unsigned mode,
    const way_identity_t *identity, way_served_t served, bool clean, int requests)
{
    int before = nfake_requests;
    yonder_picture_t *p = image_cache_run(cache, url, mode, identity);
    check(p && p->status == OS64_IMAGE_OK && p->fetched && p->served == served &&
          p->origin_clean == clean && p->image.width == 1920 && p->image.height == 1080 &&
          p->cost == 8294400 && p->decoded && !p->reduced,
          "image cache: decoded full PNG, provenance and taint agree");
    check(nfake_requests == before + requests, "image cache: expected number of HTTP requests");
    yonder_picture_release(NULL, p);
}

static void image_cache_cases(void)
{
    size_t baseline = live;
    int64_t epoch = js_test_clock.epoch;
    size_t png_len = 0;
    uint8_t *png = NULL;
    /* Fixture file access uses the host here; cache I/O uses the fake disk. */
    FILE *file = fopen("tools/pages/image-memory.png", "rb");
    check(file != NULL, "image cache: PNG fixture opened");
    if (!file) return;
    fseek(file, 0, SEEK_END); png_len = (size_t)ftell(file); rewind(file);
    png = malloc(png_len);
    check(png && fread(png, 1, png_len, file) == png_len, "image cache: PNG fixture read");
    fclose(file);
    if (!png) return;
    fake_reset(); fake_fetch_reset();
    way_cache_t *cache = way_cache_open("/tmp/image-cache", 16 << 20);
    check(cache != NULL, "image cache: cache opened");
    const char *url = "https://b.test/image.png";
    const char *head = "Content-Type: image/png\r\nCache-Control: max-age=60\r\nETag: \"image\"\r\n";
    fake_answer_t *answer = fake_answer("GET", url, 200, head, (const char *)png);
    answer->body_len = png_len;
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);
    char field[256];
    check(!strcmp(fake_field(0, "Sec-Fetch-Dest", field, sizeof(field)), "image"),
          "image cache: request destination is image");
    check(!strcmp(fake_field(0, "Sec-Fetch-Mode", field, sizeof(field)), "no-cors") &&
          !strcmp(fake_field(0, "Referer", field, sizeof(field)), "https://a.test/"),
          "image cache: no-cors mode and strict cross-origin referrer");
    check(strstr(fake_requests[0].accept, "image/png") && strstr(fake_requests[0].accept, "image/svg+xml"),
          "image cache: built-in Accept names supported image types");
    image_cache_expect(cache, url, 0, NULL, WAY_SERVED_KEPT, false, 0);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_KEPT, false, 0);
    js_test_clock.epoch += 120;
    answer->status = 304; answer->headers = "Cache-Control: max-age=600\r\n";
    answer->body = NULL; answer->body_len = 0;
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_CONFIRMED, false, 1);
    check(!strcmp(fake_field(1, "If-None-Match", field, sizeof(field)), "\"image\""),
          "image cache: stale reply revalidated with its ETag");

    answer->status = 200; answer->headers = head;
    answer->body = (const char *)png; answer->body_len = png_len;
    way_cache_enable(cache, false);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);
    way_cache_enable(cache, true);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_KEPT, false, 0);
    image_cache_expect(NULL, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);
    way_cache_key_t key = {"https a.test", true, url};
    way_cache_drop(cache, &key);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);

    /* Reproduce the headerless entry written by the legacy display fetch path,
     * including a later overwrite after native fetching populated it. */
    for (int repeat = 0; repeat < 2; repeat++) {
        way_entry_t entry; way_blocks_t blocks; uint8_t *bytes = NULL; size_t len = 0;
        bool found = way_cache_find(cache, &key, &entry, &bytes, &len, &blocks, OS64_IMAGE_CAP_DEFAULT);
        check(found && blocks.headers_len > 0, "image cache: native entry carries response headers");
        check(found && way_cache_put(cache, &entry, NULL, bytes, len),
              "image cache: seed legacy display-format headerless overwrite");
        os64_free(bytes);
        image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);
        image_cache_expect(cache, url, 1, NULL, WAY_SERVED_KEPT, false, 0);
    }

    /* Anonymous requests use a separate credential key. A credentialed
     * request still checks CORS on the no-CORS response already in cache. */
    answer->headers = "Content-Type: image/png\r\nCache-Control: max-age=600\r\n"
                      "Access-Control-Allow-Origin: https://a.test\r\n";
    image_cache_expect(cache, url, 2, NULL, WAY_SERVED_NETWORK, true, 1);
    image_cache_expect(cache, url, 2, NULL, WAY_SERVED_KEPT, true, 0);
    check(!strcmp(fake_field(nfake_requests - 1, "Origin", field, sizeof(field)), "https://a.test"),
          "image cache: anonymous CORS carries the document origin");
    yonder_picture_t *p = image_cache_run(cache, url, 3, NULL);
    check(p && p->status != OS64_IMAGE_OK && !p->origin_clean,
          "image cache: cache and network cannot bypass credentialed CORS");
    yonder_picture_release(NULL, p);
    answer->headers = "Content-Type: image/png\r\nCache-Control: max-age=600\r\n"
                      "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Credentials: true\r\n";
    image_cache_expect(cache, url, 3, NULL, WAY_SERVED_NETWORK, true, 1);
    image_cache_expect(cache, url, 3, NULL, WAY_SERVED_KEPT, true, 0);

    /* Identity profiles participate in Accept and Vary; fetch's
     * generic profile must not override the image profile. */
    way_cache_drop(cache, &key);
    answer->headers = "Content-Type: image/png\r\nCache-Control: max-age=600\r\nVary: Accept\r\n";
    const way_identity_t identity = {.accept = {[WAY_DEST_IMAGE] = "image/png", [WAY_DEST_EMPTY] = "*/*"}};
    image_cache_expect(cache, url, 1, &identity, WAY_SERVED_NETWORK, false, 1);
    check(!strcmp(fake_requests[nfake_requests - 1].accept, "image/png"),
          "image cache: image identity Accept chosen");
    image_cache_expect(cache, url, 1, &identity, WAY_SERVED_KEPT, false, 0);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_KEPT, false, 0);

    way_cache_drop(cache, &key);
    answer->headers = "Content-Type: image/png\r\nCache-Control: no-store\r\n";
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);
    image_cache_expect(cache, url, 1, NULL, WAY_SERVED_NETWORK, false, 1);

    /* Cross-origin detours stay tainted even when the final URL is local
     * to the document's origin; redirect chains are not cached here. */
    fake_answer("GET", "https://a.test/start.png", 302, "Location: https://b.test/hop\r\n", NULL);
    fake_answer("GET", "https://b.test/hop", 302, "Location: https://a.test/end.png\r\n", NULL);
    fake_answer_t *end = fake_answer("GET", "https://a.test/end.png", 200, head, (const char *)png);
    end->body_len = png_len;
    image_cache_expect(cache, "https://a.test/start.png", 1, NULL, WAY_SERVED_NETWORK, false, 3);
    image_cache_expect(cache, "https://a.test/start.png", 1, NULL, WAY_SERVED_NETWORK, false, 3);
    image_cache_expect(cache, "https://a.test/end.png", 1, NULL, WAY_SERVED_NETWORK, true, 1);
    image_cache_expect(cache, "https://a.test/end.png", 1, NULL, WAY_SERVED_KEPT, true, 0);
    way_cache_close(cache);
    fake_fetch_reset(); fake_reset(); free(png); js_test_clock.epoch = epoch;
    check(live == baseline, "image cache: worker, response and cache ownership released");
}
