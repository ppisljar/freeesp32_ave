/* =========================================================================
   freeesp32_ave — project site behaviour
   =========================================================================

   The ONLY script on the site. Like the stylesheet it makes no network
   requests, so a local checkout still works with the network unplugged.

   Everything here is progressive enhancement: with JavaScript off you lose the
   contents rail, the heading anchors and the film grain, and the pages read
   exactly as they always did. Nothing here is load-bearing for the content.

   WHY THE CONTENTS RAIL IS GENERATED. Ten pages, each with its own set of
   sections, and the old hand-written "On this page" lists had already drifted
   out of step with their headings. Deriving the rail from the h2/h3 that are
   actually in the document means it can never be wrong, and adding a section
   to a page is one edit instead of two.
   ========================================================================= */

(function () {
  'use strict';

  /* ---- film grain ------------------------------------------------------
     Injected rather than written into every page: it is pure decoration and
     has no business in the markup. */
  var grain = document.createElement('div');
  grain.className = 'grain';
  grain.setAttribute('aria-hidden', 'true');
  document.body.appendChild(grain);

  var main = document.querySelector('main.page');
  var toc  = document.getElementById('toc');
  if (!main || !toc) return;

  /* ---- slugs -----------------------------------------------------------
     Only used for headings that have no id of their own. Hand-written ids in
     the markup always win, because they are what existing cross-page links
     point at and silently renaming one would break them. */
  var used = Object.create(null);
  function slug(text) {
    var base = text.toLowerCase()
      .replace(/[^\w\s-]/g, '')
      .trim()
      .replace(/\s+/g, '-')
      .slice(0, 48) || 'section';
    var s = base, n = 2;
    while (used[s] || document.getElementById(s)) { s = base + '-' + n++; }
    used[s] = true;
    return s;
  }

  var headings = main.querySelectorAll('h2, h3');
  if (!headings.length) { toc.remove(); return; }

  /* ---- build the list -------------------------------------------------- */
  var list = document.createElement('ul');
  var links = [];
  var targets = [];

  Array.prototype.forEach.call(headings, function (h) {
    // A heading explicitly marked .no-toc stays out of the rail (used for
    // headings inside cards and other non-structural furniture).
    if (h.classList.contains('no-toc')) return;
    if (!h.id) h.id = slug(h.textContent || '');

    var li = document.createElement('li');
    li.className = 'lvl-' + h.tagName.charAt(1);

    var a = document.createElement('a');
    a.href = '#' + h.id;
    a.textContent = (h.textContent || '').replace(/\s*¶\s*$/, '').trim();
    li.appendChild(a);
    list.appendChild(li);

    links.push(a);
    targets.push(h);

    // Heading anchor, revealed on hover by the stylesheet.
    var anchor = document.createElement('a');
    anchor.className = 'anchor';
    anchor.href = '#' + h.id;
    anchor.textContent = '#';
    anchor.setAttribute('aria-label', 'Link to this section');
    h.appendChild(anchor);
  });

  if (!links.length) { toc.remove(); return; }

  var title = document.createElement('p');
  title.className = 'toc-title';
  title.textContent = 'On this page';

  // The mobile disclosure. Hidden on wide screens by the stylesheet, where the
  // rail is always open.
  var toggle = document.createElement('button');
  toggle.type = 'button';
  toggle.className = 'toc-toggle';
  toggle.textContent = 'On this page';
  toggle.setAttribute('aria-expanded', 'false');

  var body = document.createElement('div');
  body.className = 'toc-body';
  body.appendChild(list);

  toc.setAttribute('aria-label', 'On this page');
  toc.setAttribute('data-open', 'false');
  toc.appendChild(toggle);
  toc.appendChild(title);
  toc.appendChild(body);

  toggle.addEventListener('click', function () {
    var open = toc.getAttribute('data-open') === 'true';
    toc.setAttribute('data-open', open ? 'false' : 'true');
    toggle.setAttribute('aria-expanded', open ? 'false' : 'true');
  });

  /* ---- scroll spy ------------------------------------------------------
     Position-based rather than IntersectionObserver-based on purpose. An
     observer tells you which headings are *visible*, which is not the same
     question: scrolled to the middle of a long section no heading is on
     screen at all and the rail would highlight nothing. What we want is "the
     last heading we scrolled past", which is a sort, not an intersection. */
  var current = null;
  function spy() {
    var line = (window.scrollY || window.pageYOffset) +
               (parseFloat(getComputedStyle(document.documentElement).scrollPaddingTop) || 80) + 8;
    var idx = 0;
    for (var i = 0; i < targets.length; i++) {
      if (targets[i].getBoundingClientRect().top + window.scrollY <= line) idx = i;
      else break;
    }
    // At the very bottom the last section may be too short to ever cross the
    // line; treat "scrolled to the end" as being in the final section.
    if (window.innerHeight + window.scrollY >= document.body.scrollHeight - 2) {
      idx = targets.length - 1;
    }
    var a = links[idx];
    if (a === current) return;
    if (current) current.classList.remove('is-current');
    a.classList.add('is-current');
    current = a;

    // Keep the active entry in view inside the rail's own scroller, but only
    // when the rail is actually a scroller (wide screens).
    if (toc.scrollHeight > toc.clientHeight + 4) {
      var r = a.getBoundingClientRect(), t = toc.getBoundingClientRect();
      if (r.top < t.top + 4) toc.scrollTop -= (t.top - r.top) + 16;
      else if (r.bottom > t.bottom - 4) toc.scrollTop += (r.bottom - t.bottom) + 16;
    }
  }

  var ticking = false;
  function onScroll() {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(function () { spy(); ticking = false; });
  }

  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', onScroll, { passive: true });
  spy();
})();
