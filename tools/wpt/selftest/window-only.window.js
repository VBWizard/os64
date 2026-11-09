test(function() {
  assert_equals(typeof GLOBAL, "undefined");
}, "a .window.js runs as WPT's window page, which sets no GLOBAL");
