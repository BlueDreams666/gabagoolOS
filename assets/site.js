// Light and dark: follows the system until you choose; the choice stays on this device only.
(function () {
  var root = document.documentElement;
  var btn = document.getElementById('theme');
  if (!btn) return;
  function current() {
    return root.dataset.theme || (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
  }
  function label() {
    btn.setAttribute('aria-label', current() === 'light' ? 'Switch to dark theme' : 'Switch to light theme');
  }
  btn.addEventListener('click', function () {
    var next = current() === 'light' ? 'dark' : 'light';
    root.dataset.theme = next;
    try { localStorage.setItem('theme', next); } catch (e) {}
    label();
  });
  label();
})();

// The moving parts. All of it waits for reduced motion to be off.
(function () {
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;

  // the Songs list scrolls inside its R1 while the page scrolls past it
  var lists = Array.prototype.slice.call(document.querySelectorAll('.scroller'));
  var ticking = false;
  function scrollLists() {
    ticking = false;
    var vh = innerHeight;
    lists.forEach(function (box) {
      var win = box.querySelector('.list-window'), strip = win && win.querySelector('img');
      if (!strip) return;
      var r = box.getBoundingClientRect();
      var p = Math.min(1, Math.max(0, (vh - r.top) / (vh + r.height)));
      var travel = Math.max(0, strip.offsetHeight - win.offsetHeight);
      strip.style.setProperty('--list', (-travel * p * 0.9).toFixed(1) + 'px');
    });
  }
  function onScroll() { if (!ticking) { ticking = true; requestAnimationFrame(scrollLists); } }
  if (lists.length) { addEventListener('scroll', onScroll, { passive: true }); addEventListener('resize', onScroll); addEventListener('load', onScroll); onScroll(); }

  // each R1 catches the light under a mouse or trackpad
  if (matchMedia('(hover: hover) and (pointer: fine)').matches) {
    Array.prototype.forEach.call(document.querySelectorAll('.feature'), function (sec) {
      var r1s = sec.querySelectorAll('.r1');
      if (!r1s.length) return;
      var rim = getComputedStyle(r1s[0]).getPropertyValue('--rim').trim();
      sec.addEventListener('pointermove', function (e) {
        var bs = document.body.style;
        bs.setProperty('--wash', rim);
        bs.setProperty('--wx', e.clientX + 'px');
        bs.setProperty('--wy', e.clientY + 'px');
        document.body.classList.add('lit');
        sec.classList.add('lit');
        Array.prototype.forEach.call(r1s, function (r1) {
          var d = r1.getBoundingClientRect();
          var x = (e.clientX - d.left) / d.width, y = (e.clientY - d.top) / d.height;
          var cx = Math.max(-1, Math.min(1, x * 2 - 1)), cy = Math.max(-1, Math.min(1, y * 2 - 1));
          r1.style.setProperty('--ry', (cx * 4).toFixed(2) + 'deg');
          r1.style.setProperty('--rx', (-cy * 3).toFixed(2) + 'deg');
          r1.style.setProperty('--gx', (x * 100).toFixed(1) + '%');
          r1.style.setProperty('--gy', (y * 100).toFixed(1) + '%');
        });
      });
      sec.addEventListener('pointerleave', function () {
        sec.classList.remove('lit');
        document.body.classList.remove('lit');
        Array.prototype.forEach.call(r1s, function (r1) {
          ['--rx', '--ry', '--gx', '--gy'].forEach(function (k) { r1.style.removeProperty(k); });
        });
      });
    });
  }

  // sections slide up a little as they arrive (only those still below the fold)
  if ('IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (es) {
      es.forEach(function (e) { if (e.isIntersecting) { e.target.classList.add('in'); e.target.classList.remove('pre'); io.unobserve(e.target); } });
    }, { rootMargin: '0px 0px -12% 0px' });
    Array.prototype.forEach.call(document.querySelectorAll('.feature .copy, .feature .shot, .special-card, .safe li, .install li'), function (el) {
      if (el.getBoundingClientRect().top > innerHeight) { el.classList.add('pre'); io.observe(el); }
    });
  }
  // and a plain scroll check, in case the observer is slow to say so
  function reveal() {
    Array.prototype.forEach.call(document.querySelectorAll('.pre'), function (el) {
      if (el.getBoundingClientRect().top < innerHeight * 0.92) { el.classList.add('in'); el.classList.remove('pre'); }
    });
  }
  addEventListener('scroll', function () { requestAnimationFrame(reveal); }, { passive: true });
})();

// R1s you can try: tap the screen (or a dot) for the next screen, as the player would slide it in.
(function () {
  var still = matchMedia('(prefers-reduced-motion: reduce)').matches;
  Array.prototype.forEach.call(document.querySelectorAll('.try[data-screens]'), function (fig) {
    var screens = fig.dataset.screens.split(';').map(function (s) { var p = s.split('|'); return { src: p[0], name: p[1] }; });
    var btn = fig.querySelector('.screen'), name = fig.querySelector('.try-name'), dots = fig.querySelector('.dots');
    var at = 0, busy = false;
    screens.forEach(function (s, i) {   // warm the next screens so the slide never waits
      if (i) { var im = new Image(); im.src = s.src; }
      var d = document.createElement('button');
      d.type = 'button'; d.setAttribute('aria-label', s.name);
      d.addEventListener('click', function (e) { e.stopPropagation(); go(i); });
      dots.appendChild(d);
    });
    function mark() {
      Array.prototype.forEach.call(dots.children, function (d, i) { d.setAttribute('aria-current', i === at ? 'true' : 'false'); });
      name.textContent = screens[at].name;
    }
    function go(i) {
      if (busy || i === at) return;
      fig.classList.add('tried');
      var old = btn.querySelector('img'), img = new Image();
      img.src = screens[i].src; img.alt = screens[i].name; img.width = 480; img.height = 800;
      at = i; mark();
      if (still) { btn.replaceChild(img, old); return; }
      busy = true;
      img.className = 'in-right'; old.className = 'out-left';
      btn.appendChild(img);
      setTimeout(function () { old.remove(); img.className = ''; busy = false; }, 440);
    }
    btn.addEventListener('click', function () { go((at + 1) % screens.length); });
    mark();
  });

  // Your taste: skip flies left, keep flies right, love flies up, undo brings the last one back;
  // each swipe deals the next song, five in all, round again after the last
  Array.prototype.forEach.call(document.querySelectorAll('.taste'), function (fig) {
    var card = fig.querySelector('.card'), base = fig.querySelector('.screen > img'), busy = false;
    var n = +fig.dataset.cards || 1, at = 1;
    function src(kind, k) { return 'assets/shots/gh-taste' + (kind ? '-card-' : '-') + k + '.png'; }
    for (var k = 2; k <= n; k++) { new Image().src = src(0, k); new Image().src = src(1, k); }
    function play(cls) {
      if (busy) return;
      fig.classList.add('tried');
      var back = cls === 'come-back', next = back ? (at === 1 ? n : at - 1) : (at === n ? 1 : at + 1);
      if (still) { base.src = src(0, next); at = next; return; }
      busy = true;
      if (back) { card.src = src(1, next); }
      else { card.src = src(1, at); base.src = src(0, next); }
      card.className = 'card ' + cls;
      setTimeout(function () {
        if (back) base.src = src(0, next);
        at = next;
        card.className = 'card'; base.classList.remove('pop'); void base.offsetWidth; base.classList.add('pop'); busy = false;
      }, back ? 460 : 560);
    }
    fig.querySelector('.skip').addEventListener('click', function () { play('go-left'); });
    fig.querySelector('.keep').addEventListener('click', function () { play('go-right'); });
    fig.querySelector('.love').addEventListener('click', function () { play('go-up'); });
    fig.querySelector('.undo').addEventListener('click', function () { play('come-back'); });
  });
})();
