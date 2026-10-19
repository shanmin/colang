// Co docs language switcher: inserts a link in the mdBook top bar pointing to the
// matching page in the other language. Relies on zh/ and en/ keeping the same file
// layout, mapping URLs by replacing /colang/zh/ <-> /colang/en/.
(function () {
  function init() {
    var bar = document.querySelector('.right-buttons') || document.getElementById('menu-bar');
    if (!bar || document.getElementById('lang-toggle')) { return; }

    var path = location.pathname;
    var m = path.match(/\/colang\/(zh|en)\//);
    if (!m) { return; }

    var cur = m[1];
    var other = cur === 'zh' ? 'en' : 'zh';
    var label = other === 'zh' ? '中文' : 'English';
    var href = path.replace('/colang/' + cur + '/', '/colang/' + other + '/') + location.hash;

    var a = document.createElement('a');
    a.id = 'lang-toggle';
    a.className = 'lang-toggle-button';
    a.href = href;
    a.textContent = label;
    a.title = '切换语言 / Switch language';
    bar.appendChild(a);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
