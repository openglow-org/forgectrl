/*
 * login.js - forgectrl: the panel login
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
(function () {
  var f = document.getElementById('lg'),
    msg = document.getElementById('msg'),
    go = document.getElementById('go');
  fetch('/settings')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      document.getElementById('host').textContent = s.machine_id || '';
    })
    .catch(function () {});
  /* Where the login returns to: the same-origin path that sent the
   * browser here (?next=, kept with any fragment), else the panel. */
  function nextPath() {
    var n = new URLSearchParams(location.search).get('next') || '/';
    if (n.charAt(0) !== '/' || n.charAt(1) === '/' || n.charAt(1) === '\\') n = '/';
    if (location.hash && n.indexOf('#') < 0) n += location.hash;
    return n;
  }
  f.addEventListener('submit', function (e) {
    e.preventDefault();
    var body = new URLSearchParams();
    body.set('name', document.getElementById('name').value.trim());
    body.set('password', document.getElementById('password').value);
    go.disabled = true;
    msg.className = 'msg';
    msg.textContent = 'signing in…';
    fetch('/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString()
    })
      .then(function (r) {
        return r.json().then(function (j) {
          return { ok: r.ok, status: r.status, j: j };
        });
      })
      .then(function (x) {
        if (x.ok) {
          location.replace(nextPath());
          return;
        }
        msg.className = 'msg bad';
        msg.textContent = (x.j && x.j.error) || 'sign-in failed';
        if (x.status === 429 && x.j && x.j.wait) {
          var left = x.j.wait;
          var t = setInterval(function () {
            left--;
            if (left <= 0) {
              clearInterval(t);
              msg.textContent = '';
              go.disabled = false;
            } else msg.textContent = 'too many attempts; wait ' + left + ' s';
          }, 1000);
          return;
        }
        go.disabled = false;
      })
      .catch(function () {
        msg.className = 'msg bad';
        msg.textContent = 'no answer from the machine';
        go.disabled = false;
      });
  });
})();
