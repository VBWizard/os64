// META: title=The runner's own: a script test
// META: script=support/helper.js
'use strict';
test(function() {
  assert_true(GLOBAL.isWindow());
  assert_array_equals(loaded, ["helper"]);
}, "a .any.js runs as WPT's window page, its META scripts first");
