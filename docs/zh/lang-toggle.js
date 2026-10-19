// Co 文档语言切换：在 mdBook 顶栏右侧按钮区插入一个指向对应另一语言页面的链接。
// 依赖 zh/ 与 en/ 两套 book 保持相同的文件结构，通过替换 URL 中的 /colang/zh/ <-> /colang/en/ 实现映射。
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
