/* os64's testharnessreport.js: the seam web-platform-tests leaves empty for
 * the browser running it to fill (MODERN_WEB.md § The oracle). The WPT
 * runner (tools/test_yonder_wpt_host.c) serves this file in place of the
 * suite's own for /resources/testharnessreport.js.
 *
 * The results leave the page through the document: when the harness
 * completes, a <pre id="os64-wpt-results"> is appended whose text is one
 * line for the harness (`HARNESS`, its status, its message) and one per
 * test (its status, its name, its message), tab-separated, and the runner
 * reads that element out of the tree once the loop settles. A backslash,
 * tab, carriage return, newline or NUL in a name or a message is written
 * \\, \t, \r, \n or \0, so a line is always one test, and all of it.
 *
 * The harness's own output is off: it builds its table of results with
 * parts of the DOM a page under test may be the first to need, and the
 * runner reads the results from here, not from that table.
 *
 * A harness that ended in ERROR because the page itself threw or left a
 * rejection unhandled is told apart from one that ended in ERROR for any
 * other reason (Opus's ruling, 2026-10-24, matching wpt.fyi): its status is
 * written PAGE_ERROR, and the runner counts the tests the page completed.
 * It is told by what testharness.js reported, nothing else: the harness's
 * status is ERROR, and its message is the one testharness.js's own
 * listeners make of an `error` or `unhandledrejection` event this page
 * raised at window, which the capturing listeners below, running before
 * them, wrote down. The rejection's message is made as testharness.js
 * makes it at the pin. */
setup({output: false});

var os64_page_errors = Object.create(null);
addEventListener("error", function (e) {
    if (e.target === self) os64_page_errors[e.message] = true;
}, true);
addEventListener("unhandledrejection", function (e) {
    if (e.target !== self) return;
    var message = "Unhandled rejection";
    if (e.reason && e.reason.message) message = "Unhandled rejection: " + e.reason.message;
    else if (typeof e.reason === "string" && e.reason) message = "Unhandled rejection: " + e.reason;
    os64_page_errors[message] = true;
}, true);

add_completion_callback(function (tests, harness_status) {
    var test_names = ["PASS", "FAIL", "TIMEOUT", "NOTRUN", "PRECONDITION_FAILED"];
    var harness_names = ["OK", "ERROR", "TIMEOUT", "PRECONDITION_FAILED"];
    var harness_name = harness_names[harness_status.status];
    if (harness_status.status === harness_status.ERROR && os64_page_errors[harness_status.message] === true)
        harness_name = "PAGE_ERROR";
    function one_line(text) {
        return String(text === undefined || text === null ? "" : text)
            .replace(/\\/g, "\\\\").replace(/\t/g, "\\t")
            .replace(/\r/g, "\\r").replace(/\n/g, "\\n").replace(/\0/g, "\\0");
    }
    var lines = ["HARNESS\t" + harness_name + "\t" +
                 one_line(harness_status.message)];
    for (var i = 0; i < tests.length; i++)
        lines.push(test_names[tests[i].status] + "\t" + one_line(tests[i].name) + "\t" +
                   one_line(tests[i].message));
    var out = document.createElement("pre");
    out.id = "os64-wpt-results";
    out.textContent = lines.join("\n") + "\n";
    (document.body || document.documentElement).appendChild(out);
});
