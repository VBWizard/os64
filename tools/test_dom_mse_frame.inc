/* MEDIA SOURCE EXTENSIONS ACROSS A FRAME (libdom/mse.c): a parent window
 * reading a frame's SourceBufferList is answered by the list's own binding,
 * not the reading realm's, even when the parent has no Media Source
 * Extensions at all. The child-install fixture's frame is published to the
 * parent as C; the frame has the switch, the fake hosts of
 * test_dom_mse.inc, and an attached source with one SourceBuffer. Run by
 * --mse, after test_dom_mse.inc's cases. */

static void dom_mse_frame_cases(void)
{
    ChildInstallFixture f;
    int policy = OS64_JS_TEARDOWN_FATAL;
    if (!child_install_open(&f, policy, true)) {
        child_install_close(&f, policy);
        return;
    }
    memset(fed_sessions, 0, sizeof(fed_sessions));
    fed_opened = fed_closed = 0;
    memset(media_sessions, 0, sizeof(media_sessions));
    media_opened = media_closed = 0;
    /* The parent was made without the switch; the frame has it. */
    f.options.media_source = true;
    f.options.entropy = mse_entropy;
    os64_js_outcome_t out;
    f.pending = child_install_document();
    os64_dom_child_t child = child_install_attempt(&f, &out);
    check(child.state == OS64_DOM_CHILD_ACTIVE && child.consumed, "mse frame: the frame's document installed");
    child_install_unconsumed(&f.pending, child);
    if (child.state == OS64_DOM_CHILD_ACTIVE) {
        os64_dom_set_media(child.binding, &media_provider);
        os64_dom_set_fed(child.binding, &fed_provider);
        child_install_publish(&f, child, "C");
        realm_script(child.realm, STORAGE_ASSERT "globalThis.v=document.createElement('video');globalThis.ms=new MediaSource();"
                     "v.src=URL.createObjectURL(ms);",
                     "mse frame: a source in the frame");
        for (int turn = 0; turn < 2; turn++) {
            os64_dom_media_turn(child.binding);
            for (int i = 0; i < 64 && os64_dom_media_fire(child.binding, &out); i++)
                check(out.status == OS64_JS_OK, "mse frame: the frame's media turn");
            if (turn == 0)
                realm_script(child.realm, STORAGE_ASSERT "assert(ms.readyState==='open','open');"
                             "ms.addSourceBuffer('video/mp4; codecs=\"avc1.64001f\"');",
                             "mse frame: a SourceBuffer in the frame");
        }
        realm_script(f.parent, STORAGE_ASSERT "assert(typeof MediaSource==='undefined','the parent has none');"
                     "const l=C.ms.sourceBuffers;"
                     "assert(l.length===1&&l[0]!==undefined&&l[0]===C.ms.sourceBuffers[0],'the frame\\'s list, read by the parent');"
                     "assert(Object.keys(l).join()==='0'&&[...l].length===1,'its indices enumerate');",
                     "mse frame: the parent reads the frame's list");
    }
    child_install_close(&f, policy);
}
