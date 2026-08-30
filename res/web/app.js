/* Over The Hare, the browser half.
 *
 * No framework and no build step: this file is served off the card exactly as
 * it is written, so it can be edited on the device and reloaded. That is worth
 * more here than any convenience a toolchain would add - the whole point of
 * the feature is not needing a card reader.
 *
 * Uploads go through XMLHttpRequest rather than fetch, for one reason:
 * upload progress. fetch still has no way to report how much of a request body
 * has gone out, and a 900 MB ROM with no progress bar is indistinguishable
 * from a hang.
 */
'use strict';

const $ = (id) => document.getElementById(id);

let cwd = '';                 /* the path the listing is showing, "" = roots */
let entries = [];
let roots = [];               /* remembered from the root listing, for crumbs */

/* ---- talking to the device -------------------------------------------- */

/* Every non-200 is an error with the server's own words in it. The routes
 * answer in plain text precisely so this can show them without a schema. */
async function api(method, path, body) {
	const r = await fetch(path, { method, body, credentials: 'same-origin' });
	if (r.status === 401) { showGate('Session ended. Enter the PIN again.'); throw new Error('unauthorized'); }
	if (!r.ok) throw new Error((await r.text()).split('\n')[1] || r.statusText);
	return r;
}

const enc = encodeURIComponent;

/* ---- the gate ---------------------------------------------------------- */

function showGate(msg) {
	$('app').hidden = true;
	$('gate').hidden = false;
	$('gatemsg').textContent = msg || '';
	$('gatemsg').classList.remove('ok');
	$('pin').value = '';
	$('pin').focus();
}

$('pinform').addEventListener('submit', async (e) => {
	e.preventDefault();
	const pin = $('pin').value.trim();
	if (pin.length !== 4) { $('gatemsg').textContent = 'Four digits.'; return; }
	try {
		const r = await fetch('/api/auth', { method: 'POST', body: pin });
		if (r.status === 429) {
			$('gatemsg').textContent = 'Too many wrong PINs. Wait half a minute.';
			return;
		}
		if (!r.ok) { $('gatemsg').textContent = 'Wrong PIN.'; $('pin').value = ''; return; }
		$('gate').hidden = true;
		$('app').hidden = false;
		await go('');
	} catch (err) {
		$('gatemsg').textContent = 'Could not reach the device.';
	}
});

/* ---- listing ----------------------------------------------------------- */

function human(n) {
	if (n < 1024) return n + ' B';
	const u = ['KB', 'MB', 'GB'];
	let i = -1;
	do { n /= 1024; i++; } while (n >= 1024 && i < u.length - 1);
	return (n < 10 ? n.toFixed(1) : Math.round(n)) + ' ' + u[i];
}

function crumbs() {
	const nav = $('crumbs');
	nav.textContent = '';
	const parts = cwd ? cwd.split('/') : [];
	const mk = (label, path, last) => {
		if (last) {
			const s = document.createElement('span');
			s.className = 'here';
			s.textContent = label;
			nav.append(s);
			return;
		}
		const b = document.createElement('button');
		b.textContent = label;
		b.onclick = () => go(path);
		nav.append(b);
		const sep = document.createElement('span');
		sep.className = 'sep';
		sep.textContent = '/';
		nav.append(sep);
	};
	mk('Device', '', parts.length === 0);
	parts.forEach((p, i) => {
		/* The first segment is a root's URL name - "roms" - and the device
		 * calls it "ROMs" everywhere else. Show what it is called. */
		const root = i === 0 && roots.find((r) => r.path === p);
		mk(root ? root.name : p, parts.slice(0, i + 1).join('/'),
		   i === parts.length - 1);
	});
}

async function go(path) {
	const r = await api('GET', '/api/list' + (path ? '?p=' + enc(path) : ''));
	const data = await r.json();
	cwd = data.path;
	if (cwd === '') {
		/* The roots keep the order the device gave them: ROMs is the reason
		 * anyone opened this, and alphabetical put BIOS above it. */
		entries = data.entries;
		roots = data.entries.slice();
	} else {
		/* Inside a folder, sorted: vfat's readdir order is creation order,
		 * which is no order at all to somebody looking for a game. */
		entries = data.entries.sort((a, b) =>
			(b.dir - a.dir) || a.name.localeCompare(b.name, undefined, { numeric: true }));
	}
	draw();
}

function draw() {
	crumbs();
	const ul = $('list');
	ul.textContent = '';
	$('empty').hidden = entries.length > 0;

	for (const e of entries) {
		const li = document.createElement('li');
		li.className = 'row' + (e.dir ? ' dir' : '');

		const mark = document.createElement('span');
		mark.className = 'mark';
		/* Text, not an icon font and not an SVG sprite: two characters that
		 * every system font already has, and nothing more to serve. */
		mark.textContent = e.dir ? '▸' : '·';
		li.append(mark);

		if (e.dir) {
			const b = document.createElement('button');
			b.className = 'name';
			b.textContent = e.name;
			b.onclick = () => go(e.path);
			li.append(b);
		} else {
			const s = document.createElement('span');
			s.className = 'name';
			s.textContent = e.name;
			li.append(s);
			const sz = document.createElement('span');
			sz.className = 'size';
			sz.textContent = human(e.size);
			li.append(sz);
		}

		const acts = document.createElement('div');
		acts.className = 'acts';
		if (!e.dir) {
			const a = document.createElement('a');
			a.href = '/api/file?p=' + enc(e.path);
			a.textContent = 'Get';
			a.setAttribute('download', e.name);
			acts.append(a);
		}
		if (cwd) {                       /* the roots themselves are not editable */
			acts.append(mkbtn('Rename', () => rename(e)));
			const d = mkbtn('Delete', () => del(e));
			d.className = 'danger';
			acts.append(d);
		}
		li.append(acts);
		ul.append(li);
	}
}

function mkbtn(label, fn) {
	const b = document.createElement('button');
	b.textContent = label;
	b.onclick = fn;
	return b;
}

/* ---- the operations ---------------------------------------------------- */

async function rename(e) {
	const to = prompt('Rename to', e.name);
	if (!to || to === e.name) return;
	try {
		await api('POST', '/api/rename?p=' + enc(e.path) + '&to=' + enc(to));
		toast('Renamed');
		await go(cwd);
	} catch (err) { toast(err.message, true); }
}

async function del(e) {
	if (!confirm('Delete ' + e.name + '?')) return;
	try {
		await api('POST', '/api/delete?p=' + enc(e.path));
		toast('Deleted');
		await go(cwd);
	} catch (err) { toast(err.message, true); }
}

$('newfolder').onclick = async () => {
	if (!cwd) { toast('Pick a folder first', true); return; }
	const name = prompt('New folder name');
	if (!name) return;
	try {
		await api('POST', '/api/mkdir?p=' + enc(cwd + '/' + name));
		await go(cwd);
	} catch (err) { toast(err.message, true); }
};

/* ---- uploading --------------------------------------------------------- */

const queue = [];
let sending = false;

function enqueue(files) {
	if (!cwd) { toast('Open a folder first', true); return; }
	for (const f of files) queue.push({ file: f, pct: 0, state: 'waiting' });
	drawQueue();
	if (!sending) next();
}

function next() {
	const job = queue.find((j) => j.state === 'waiting');
	if (!job) {
		sending = false;
		/* Left on screen for a moment so the last line is readable, rather
		 * than the panel vanishing the instant the final byte lands. */
		setTimeout(() => { if (!queue.some((j) => j.state === 'sending')) {
			queue.length = 0; drawQueue();
		} }, 2500);
		go(cwd).catch(() => {});
		return;
	}
	sending = true;
	job.state = 'sending';
	drawQueue();

	const xhr = new XMLHttpRequest();
	xhr.open('PUT', '/api/file?p=' + enc(cwd + '/' + job.file.name));
	xhr.upload.onprogress = (ev) => {
		if (!ev.lengthComputable) return;
		job.pct = Math.round(ev.loaded / ev.total * 100);
		drawQueue();
	};
	xhr.onload = () => {
		if (xhr.status === 200) { job.state = 'done'; job.pct = 100; }
		else {
			job.state = 'failed';
			job.why = (xhr.responseText || '').split('\n')[1] || ('HTTP ' + xhr.status);
		}
		drawQueue();
		next();
	};
	xhr.onerror = () => {
		job.state = 'failed';
		job.why = 'connection lost';
		drawQueue();
		next();
	};
	xhr.send(job.file);
}

function drawQueue() {
	const ul = $('queuelist');
	$('queue').hidden = queue.length === 0;
	ul.textContent = '';
	for (const j of queue) {
		const li = document.createElement('li');
		if (j.state === 'failed') li.className = 'failed';
		const top = document.createElement('div');
		top.className = 'top';
		const n = document.createElement('span');
		n.className = 'n';
		n.textContent = j.file.name;
		const p = document.createElement('span');
		p.className = 'pct';
		p.textContent = j.state === 'failed' ? j.why
		              : j.state === 'done' ? 'done'
		              : j.state === 'waiting' ? 'waiting'
		              : j.pct + '%';
		top.append(n, p);
		const bar = document.createElement('div');
		bar.className = 'bar';
		const fill = document.createElement('span');
		fill.style.width = (j.state === 'failed' ? 100 : j.pct) + '%';
		bar.append(fill);
		li.append(top, bar);
		ul.append(li);
	}
}

$('picker').onchange = (e) => { enqueue(e.target.files); e.target.value = ''; };

/* Drag and drop, counted rather than toggled: dragenter and dragleave fire for
 * every child element the pointer crosses, so a boolean flickers the overlay
 * off the moment the pointer moves over a row inside it. */
let dragDepth = 0;
addEventListener('dragenter', (e) => {
	e.preventDefault();
	if (++dragDepth === 1 && !$('app').hidden) $('drop').hidden = false;
});
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('drop').hidden = true; } });
addEventListener('drop', (e) => {
	e.preventDefault();
	dragDepth = 0;
	$('drop').hidden = true;
	if (e.dataTransfer.files.length) enqueue(e.dataTransfer.files);
});

/* ---- toast ------------------------------------------------------------- */

let toastTimer = 0;
function toast(msg, bad) {
	const t = $('toast');
	t.textContent = msg;
	t.classList.toggle('bad', !!bad);
	t.classList.add('show');
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => t.classList.remove('show'), 2600);
}

/* A reload should not always mean re-entering the PIN: the cookie may still be
 * good. Ask for the roots, and only show the gate if that is refused. */
(async () => {
	try {
		const r = await fetch('/api/list', { credentials: 'same-origin' });
		if (!r.ok) throw new Error();
		$('gate').hidden = true;
		$('app').hidden = false;
		await go('');
	} catch (e) {
		showGate('');
	}
})();
