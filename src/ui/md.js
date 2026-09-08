/*
 * md.js - forgectrl: a small markdown renderer for the advisory documents
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The documents under docs/advisories/ use a deliberately small subset:
 * headings (# to ###), paragraphs, bullet lists, tables, bold, italic,
 * inline code, and links. Everything is HTML-escaped first, so the
 * renderer never emits markup the document did not ask for. The output
 * is a plain HTML fragment for a container the page styles.
 */
function mdEsc(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
function mdInline(s) {
  s = mdEsc(s);
  s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
  s = s.replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>');
  s = s.replace(/(^|[^*])\*([^*]+)\*/g, '$1<i>$2</i>');
  s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, function (m, t, u) {
    return '<a href="' + u + '" target="_blank" rel="noopener">' + t + '</a>';
  });
  return s;
}
function mdRender(text) {
  var lines = text.replace(/\r\n?/g, '\n').split('\n'),
    out = [],
    i = 0,
    para = [];
  function flush() {
    if (para.length) {
      out.push('<p>' + mdInline(para.join(' ')) + '</p>');
      para = [];
    }
  }
  while (i < lines.length) {
    var l = lines[i],
      m;
    if (/^\s*$/.test(l)) {
      flush();
      i++;
      continue;
    }
    if ((m = /^(#{1,3})\s+(.*)$/.exec(l))) {
      flush();
      var n = m[1].length;
      out.push('<h' + n + '>' + mdInline(m[2]) + '</h' + n + '>');
      i++;
      continue;
    }
    if (/^\s*[-*]\s+/.test(l)) {
      flush();
      out.push('<ul>');
      while (i < lines.length && /^\s*[-*]\s+/.test(lines[i])) {
        var item = lines[i].replace(/^\s*[-*]\s+/, '');
        i++;
        while (i < lines.length && /^\s{2,}\S/.test(lines[i]) && !/^\s*[-*]\s+/.test(lines[i])) {
          item += ' ' + lines[i].trim();
          i++;
        }
        out.push('<li>' + mdInline(item) + '</li>');
      }
      out.push('</ul>');
      continue;
    }
    if (/^\s*\|/.test(l)) {
      flush();
      var rows = [];
      while (i < lines.length && /^\s*\|/.test(lines[i])) {
        rows.push(lines[i]);
        i++;
      }
      var cells = function (r) {
        return r
          .trim()
          .replace(/^\||\|$/g, '')
          .split('|')
          .map(function (c) {
            return c.trim();
          });
      };
      var html = '<table>';
      var hdr = cells(rows[0]);
      html += '<thead><tr>' + hdr.map(function (c) { return '<th>' + mdInline(c) + '</th>'; }).join('') + '</tr></thead><tbody>';
      for (var r = 1; r < rows.length; r++) {
        if (/^\s*\|?\s*:?-{2,}/.test(rows[r])) continue;
        html += '<tr>' + cells(rows[r]).map(function (c) { return '<td>' + mdInline(c) + '</td>'; }).join('') + '</tr>';
      }
      html += '</tbody></table>';
      out.push(html);
      continue;
    }
    para.push(l.trim());
    i++;
  }
  flush();
  return out.join('\n');
}
