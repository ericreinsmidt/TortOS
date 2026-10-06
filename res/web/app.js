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
		await start();
	} catch (err) {
		$('gatemsg').textContent = 'Could not reach the device.';
	}
});

/* ---- listing ----------------------------------------------------------- */

/* Decimal, not 1024.
 *
 * These were 1024-based and labeled KB/MB/GB, which is the one combination
 * that is wrong on every platform: macOS has quoted decimal since 10.6, so a
 * 15,528,261-byte ROM read 15.53 MB in Finder and 14.8 MB here. The whole job
 * of this page is to agree with the machine at the other end about what is on
 * the card, and a size that disagrees with the file manager beside it reads as
 * a failed copy. Relabeling to KiB would have been true and no help. */
function human(n) {
	if (n < 1000) return n + ' B';
	const u = ['KB', 'MB', 'GB'];
	let i = -1;
	do { n /= 1000; i++; } while (n >= 1000 && i < u.length - 1);
	/* A decimal for MB and GB, whole numbers for KB. Rounding 15,528,261 to
	 * "16 MB" put the units right and still disagreed with the file manager
	 * next to it by half a megabyte, which was the original complaint. */
	return (i === 0 ? Math.round(n) : n.toFixed(1)) + ' ' + u[i];
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

/* `hist` says what this navigation does to browser history:
 *
 *   'push'    a real navigation - a crumb or a folder, back should undo it
 *   'replace' the first listing, which is where back should stop
 *   'none'    a refresh of where you already are, after a rename, a delete,
 *             a mkdir or a finished queue. These are not navigations and
 *             pushing them would make back replay the same folder repeatedly.
 *
 * The path also goes in the fragment, so a reload lands where you were and a
 * link can be sent to another device on the LAN. */
async function go(path, hist) {
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

	const url = '#' + enc(cwd);
	if (hist === 'replace') history.replaceState({ path: cwd }, '', url);
	else if (hist !== 'none') history.pushState({ path: cwd }, '', url);
}

/* Back and forward. Always 'none': the browser has already moved its own
 * pointer through the stack, and pushing here would append a duplicate entry
 * and make forward unreachable.
 *
 * A queue in flight is unaffected, because each job carries the folder it was
 * bound to at enqueue. That ordering was deliberate - popstate fires with no
 * click and no confirmation, so shipping this while the destination was still
 * read from the live `cwd` would have turned an occasional misfile into a
 * routine one. */
addEventListener('popstate', (e) => {
	const path = e.state && typeof e.state.path === 'string' ? e.state.path : '';
	go(path, 'none').catch(() => {});
});

/* The first listing: whatever the fragment names, else the roots. Always
 * 'replace', so the entry the session opens on is the one back stops at
 * instead of leaving the page.
 *
 * A fragment naming a folder that has since been renamed or deleted is not a
 * dead session, so that falls back to the roots. An expired cookie IS, and is
 * rethrown - otherwise the retry would 401 as well and the gate would be
 * raised twice. */
async function start() {
	const want = location.hash ? decodeURIComponent(location.hash.slice(1)) : '';
	if (!want) { await go('', 'replace'); return; }
	try {
		await go(want, 'replace');
	} catch (err) {
		if (err.message === 'unauthorized') throw err;
		await go('', 'replace');
	}
}

function draw() {
	crumbs();
	/* The wording differs at the roots, where nothing takes a drop, so it has
	 * to be rebuilt whenever the listing changes rather than written once. */
	refreshDropHint();
	const ul = $('list');
	ul.textContent = '';
	$('empty').hidden = entries.length > 0;
	$('logs').hidden = cwd !== '';
	drawArt();
	/* A listing with any covers gives each row of that kind a cover's slot:
	 * every game when a game has one, every album when an album has. Albums
	 * only, not an artist's folder of them, which has no cover to show. */
	const gameCovers = entries.some((e) => !e.dir && e.cover);
	const albumCovers = entries.some((e) => e.dir && (e.cover || e.covers));
	const thumb = (path) => '/api/file?thumb=1&p=' + enc(path);

	for (const e of entries) {
		const li = document.createElement('li');
		li.className = 'row' + (e.dir ? ' dir' : '');

		if (e.dir && e.covers && e.covers.length > 1) {
			/* An artist: their albums' covers, four at most, in a square. */
			const g = document.createElement('span');
			g.className = 'cover grid';
			for (const p of e.covers.slice(0, 4)) {
				const c = document.createElement('img');
				c.src = thumb(p);
				c.alt = '';
				c.loading = 'lazy';
				g.append(c);
			}
			li.append(g);
		} else if (e.dir && e.covers) {
			const c = document.createElement('img');
			c.className = 'cover';
			c.src = thumb(e.covers[0]);
			c.alt = '';
			c.loading = 'lazy';
			li.append(c);
		} else if (e.dir ? albumCovers && (e.album || e.cover) : gameCovers) {
			/* thumb=1 so the device does not report each one as a download. */
			const c = document.createElement(e.cover ? 'img' : 'span');
			c.className = 'cover' + (e.cover ? '' : ' none');
			if (e.cover) {
				c.src = thumb(e.cover);
				c.alt = '';
				c.loading = 'lazy';
			}
			li.append(c);
		} else {
			const mark = document.createElement('span');
			mark.className = 'mark';
			/* Text, not an icon font and not an SVG sprite: two characters that
			 * every system font already has, and nothing more to serve. */
			mark.textContent = e.dir ? '▸' : '·';
			li.append(mark);
		}

		/* Drop straight onto a folder, so uploading into eleven shelves is not
		 * eleven round trips through each one.
		 *
		 * Only inside a root, never on the roots themselves. Roms/ is the
		 * reason: lib_scan is always called with a specific system folder and
		 * opens <roms_root>/<folder>, so nothing ever reads Roms/ itself - a
		 * file dropped there is never seen by any shelf again. Bios/ and
		 * Saves/ are flat and would be safe, but one uniform rule beats three
		 * special cases, and the row simply not lighting up says so without
		 * anyone having to know it. */
		if (e.dir && cwd) {
			li.classList.add('drops');
			li.addEventListener('dragover', (ev) => {
				ev.preventDefault();
				ev.stopPropagation();
				li.classList.add('over');
			});
			/* dragleave also fires crossing into a child, and relatedTarget is
			 * where the pointer went: still inside means it never left. */
			li.addEventListener('dragleave', (ev) => {
				if (li.contains(ev.relatedTarget)) return;
				li.classList.remove('over');
			});
			li.addEventListener('drop', (ev) => {
				ev.preventDefault();
				ev.stopPropagation();
				li.classList.remove('over');
				dragDepth = 0;
				/* Through dragUI, not by hiding the frame alone: the header is
				 * showing the drop message in the crumbs' place, and stopping
				 * at the frame leaves it there for good. */
				dragUI(false);
				dropped(ev.dataTransfer, e.path);
			});
		}

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
		await go(cwd, 'none');
	} catch (err) { toast(err.message, true); }
}

/* A file, or an empty folder, is asked about by name. A folder with anything
 * in it is asked about by what is in it, which the device counts first - and
 * the device also says whether it may go whole at all: an album or a book can,
 * a console's folder cannot (see xfer_delete_rule). The server enforces both;
 * this is only so the question asked is the right one. */
async function del(e) {
	if (e.dir) {
		let c;
		try {
			c = await (await api('GET', '/api/count?p=' + enc(e.path))).json();
		} catch (err) { toast(err.message, true); return; }
		if (c.files + c.folders > 0) {
			if (c.rule !== 'all') { toast(e.name + ': ' + c.why, true); return; }
			if (c.too_many) {
				toast(e.name + ' holds too much to delete at once; delete it in parts', true);
				return;
			}
			const what = c.files + (c.files === 1 ? ' file' : ' files') +
				(c.folders ? ' in ' + c.folders + (c.folders === 1 ? ' folder' : ' folders') : '');
			if (!confirm('Delete ' + e.name + ' and everything in it: ' + what + '?')) return;
			return removeEntry(e, true);
		}
	}
	if (!confirm('Delete ' + e.name + '?')) return;
	return removeEntry(e, false);
}

async function removeEntry(e, all) {
	try {
		await api('POST', '/api/delete?p=' + enc(e.path) + (all ? '&all=1' : ''));
		toast('Deleted');
		await go(cwd, 'none');
	} catch (err) { toast(err.message, true); }
}

$('newfolder').onclick = async () => {
	if (!cwd) { toast('Pick a folder first', true); return; }
	const name = prompt('New folder name');
	if (!name) return;
	try {
		await api('POST', '/api/mkdir?p=' + enc(cwd + '/' + name));
		await go(cwd, 'none');
	} catch (err) { toast(err.message, true); }
};

/* ---- uploading --------------------------------------------------------- */

const queue = [];
let sending = false;

/* `dest` is the folder these files are going to, defaulting to the one on
 * screen. It is bound HERE, into each job, rather than read at send time.
 *
 * Reading it later was a real bug: `next()` built every URL from the live
 * `cwd`, so navigating during a multi-file upload sent the rest of the queue
 * wherever you had gone. A 40-file batch, walked away from after file 5, put
 * 35 files somewhere nobody chose - and then the refresh at the end of the
 * queue listed the folder you had moved to, so the files that landed there
 * looked like the ones you meant. Binding it per job also makes a per-file
 * destination possible, which is what dropping onto a folder row uses. */
function enqueue(files, dest) {
	const dir = dest === undefined ? cwd : dest;
	if (!dir) { toast('Open a folder first', true); return; }
	for (const f of files) queue.push({ file: f, dir, pct: 0, state: 'waiting' });
	drawQueue();
	if (!sending) next();
}

/* ---- folders --------------------------------------------------------------
 *
 * A dropped folder arrives as one empty item in dataTransfer.files, which is
 * all the page used to read, so dropping an album uploaded nothing. The
 * entries API walks it instead: every folder in it is made on the device, in
 * order, parents first, and every file queued into its own folder. Dot files
 * and folders are skipped - a Mac puts .DS_Store in every folder it has shown,
 * and ._ files beside every file it copied. */

/* A tree from a drop: { dirs, files }, dirs in the order they must be made
 * and each file with the folder it belongs in, both relative to the drop.
 * null when the browser has no entries API, and the drop is read as files. */
function walkDrop(dt) {
	/* Read now: the items are only there while the drop event runs, and the
	 * walk below awaits. */
	const entries = [];
	for (const it of dt.items || []) {
		const en = it.webkitGetAsEntry && it.webkitGetAsEntry();
		if (en) entries.push(en);
	}
	if (!entries.length) return null;
	return (async () => {
		const dirs = [], files = [];
		const walk = async (en, rel) => {
			if (en.name.startsWith('.')) return;
			if (en.isFile) {
				files.push({ file: await new Promise((ok, no) => en.file(ok, no)), rel });
			} else if (en.isDirectory) {
				const here = rel ? rel + '/' + en.name : en.name;
				const reader = en.createReader();

				dirs.push(here);
				/* readEntries hands a folder over in batches, empty at the end. */
				for (;;) {
					const batch = await new Promise((ok, no) => reader.readEntries(ok, no));
					if (!batch.length) break;
					for (const c of batch) await walk(c, here);
				}
			}
		};
		for (const en of entries) await walk(en, '');
		return { dirs, files };
	})();
}

/* The same tree from the Upload folder picker, whose files carry their path
 * inside the chosen folder as webkitRelativePath. */
function picked(list) {
	const dirs = [], files = [], seen = new Set();
	for (const f of list) {
		const parts = (f.webkitRelativePath || f.name).split('/');
		if (parts.some((p) => p.startsWith('.'))) continue;
		for (let i = 1; i < parts.length; i++) {
			const d = parts.slice(0, i).join('/');
			if (!seen.has(d)) { seen.add(d); dirs.push(d); }
		}
		files.push({ file: f, rel: parts.slice(0, -1).join('/') });
	}
	return { dirs, files };
}

/* A drop, onto the list or onto a folder row: a tree when it holds folders,
 * files as before when it does not. */
function dropped(dt, dest) {
	const tree = walkDrop(dt);
	if (!tree) { if (dt.files.length) enqueue(dt.files, dest); return; }
	tree.then((t) => enqueueTree(t, dest)).catch((err) => toast(err.message, true));
}

/* Make one folder; one that is already there is fine - a second album
 * dropped into an artist's folder reuses it. */
async function makeDir(path) {
	const r = await fetch('/api/mkdir?p=' + enc(path), { method: 'POST', credentials: 'same-origin' });
	if (r.status === 401) { showGate('Session ended. Enter the PIN again.'); throw new Error('unauthorized'); }
	if (!r.ok && r.status !== 409) throw new Error((await r.text()).split('\n')[1] || r.statusText);
}

async function enqueueTree(tree, dest) {
	const dir = dest === undefined ? cwd : dest;
	if (!dir) { toast('Open a folder first', true); return; }
	if (!tree.dirs.length) { enqueue(tree.files.map((f) => f.file), dir); return; }
	try {
		for (const d of tree.dirs) await makeDir(dir + '/' + d);
	} catch (err) { toast(err.message, true); return; }
	for (const { file, rel } of tree.files)
		queue.push({ file, dir: rel ? dir + '/' + rel : dir, pct: 0, state: 'waiting' });
	if (!tree.files.length) { toast('Folder made'); go(cwd, 'none').catch(() => {}); return; }
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
		go(cwd, 'none').catch(() => {});
		return;
	}
	sending = true;
	job.state = 'sending';
	drawQueue();

	const xhr = new XMLHttpRequest();
	xhr.open('PUT', '/api/file?p=' + enc(job.dir + '/' + job.file.name));
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
		/* Where it is going, shown only when that is no longer what is on
		 * screen - after navigating away, or after a drop onto a folder row.
		 * Silent in the ordinary case, and there exactly when the answer has
		 * stopped being obvious. */
		if (j.dir !== cwd) {
			const d = document.createElement('span');
			d.className = 'dest';
			d.textContent = '→ ' + (j.dir.split('/').pop() || j.dir);
			d.title = j.dir;
			n.append(' ', d);
		}
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
$('folderpicker').onchange = (e) => {
	enqueueTree(picked(e.target.files));
	e.target.value = '';
};

/* Drag and drop, counted rather than toggled: dragenter and dragleave fire for
 * every child element the pointer crosses, so a boolean flickers the overlay
 * off the moment the pointer moves over a row inside it. */
let dragDepth = 0;

/* "here" stopped having one meaning once folder rows took drops, so the banner
 * says which of the three situations you are actually in. At the roots nothing
 * accepts a drop at all - enqueue refuses with the same words - so say that
 * before the file is let go rather than after. */
function dropLabel() {
	return cwd ? 'Drop files to upload' : 'Open a folder to upload';
}

/* The hint is permanent, not summoned by the drag.
 *
 * It used to appear only on dragenter, which meant the only person who ever saw
 * it was somebody who had already worked out that dropping was possible. A
 * capability advertised solely to people who have already found it is not
 * advertised. It sits beside the crumbs rather than replacing them, because
 * when the pointer is NOT over a folder row the crumbs are the only thing
 * saying where a drop would land.
 *
 * Dragging changes emphasis, not words. Same string lit differently: nothing
 * to keep in sync, and no flicker as the text is swapped under the pointer. */
function dragUI(on) {
	$('drop').hidden = !on;
	$('dropmsg').classList.toggle('armed', on);
}

/* Kept current as the listing changes, since the wording differs at the roots
 * where nothing accepts a drop. */
function refreshDropHint() {
	$('dropmsg').textContent = dropLabel();
}

addEventListener('dragenter', (e) => {
	e.preventDefault();
	if (++dragDepth === 1 && !$('app').hidden) dragUI(true);
});
addEventListener('dragover', (e) => e.preventDefault());
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; dragUI(false); } });
addEventListener('drop', (e) => {
	e.preventDefault();
	dragDepth = 0;
	dragUI(false);
	dropped(e.dataTransfer);
});

/* ---- box art ----------------------------------------------------------- */

/* Down To The Wire only: the device behind the cable has no network, so this
 * page fetches libretro's covers and the device decides which game gets which
 * (src/hareart.h). Where the device fetches its own, /api/art/wanted answers
 * 404 and the button never shows.
 *
 * From libretro's GitHub mirror, not thumbnails.libretro.com, which the device
 * itself uses: that server does not let a web page read what it sends, and
 * GitHub does. Its listing is a collection's whole Named_Boxarts folder, two
 * requests a collection; GitHub allows 60 an hour without an account. */
const GH_API = 'https://api.github.com/repos/libretro-thumbnails/';
const GH_RAW = 'https://raw.githubusercontent.com/libretro-thumbnails/';
/* libretro's No-Intro lists, for the checksum pass, as the device's own Box
 * Art fetches them. */
const DAT = 'https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/';
let artBusy = false;

/* libretro's repos are its collections' names with underscores for spaces:
 * "Sega - Mega Drive - Genesis" is Sega_-_Mega_Drive_-_Genesis. */
const repoOf = (collection) => collection.replace(/ /g, '_');

/* null where the device says no (404): the button stays hidden. */
async function wanted(what) {
	const r = await fetch('/api/' + what + '/wanted', { credentials: 'same-origin' });
	if (!r.ok) return null;
	return r.json();
}

/* What is missing, games and albums in one line, on the first page only, and
 * left alone while a run is saying how it is going. */
async function drawArt() {
	if (artBusy) return;
	if (cwd !== '') { $('art').hidden = true; return; }
	let games = null, albums = null;
	try { [games, albums] = await Promise.all([wanted('art'), wanted('albums')]); }
	catch (e) { /* leave it hidden */ }
	if (cwd !== '' || artBusy) return;

	const g = games ? games.shelves.reduce((n, s) => n + s.missing, 0) : 0;
	const nc = games ? games.shelves.length : 0;
	const a = albums ? albums.albums.length : 0;
	const parts = [];

	/* Both counts every time, so a zero reads as done rather than forgotten. */
	if (games) parts.push(!g ? 'Every game has a cover.' : g === 1 ? '1 game has no cover.'
		: g + ' games on ' + nc + (nc === 1 ? ' console' : ' consoles') + ' have no cover.');
	/* Album Art's own rule: no cover, or one too small to be sharp. */
	if (albums) parts.push(!a ? 'Every album has a cover.'
		: (a === 1 ? '1 album has' : a + ' albums have') + ' no cover, or a small one.');
	$('art').hidden = !games && !albums || (!g && !a && !lastRun);
	$('artgo').disabled = !g && !a;
	$('artmsg').textContent = parts.join(' ') + lastRun;
}

/* What went wrong in the last run, kept beside the count once it is over: a
 * toast is gone in seconds, and "3 failed" with no names left nothing to go
 * on (2026-10-05). */
let lastRun = '';
const failNote = (fails) => fails.length
	? ' Last time, ' + fails.length + ' failed: ' + fails.join('; ') + '.' : '';

async function github(url) {
	const r = await fetch(url);
	if (r.status === 403 || r.status === 429)
		throw new Error("GitHub's hourly limit is used up. Try again in an hour.");
	if (!r.ok) throw new Error('GitHub answered ' + r.status);
	return r.json();
}

/* A collection's box art names, without their .png. */
async function coverNames(repo) {
	const top = await github(GH_API + repo + '/git/trees/master');
	const dir = top.tree.find((e) => e.path === 'Named_Boxarts');
	if (!dir) return [];
	const box = await github(GH_API + repo + '/git/trees/' + dir.sha);
	return box.tree.filter((e) => e.path.endsWith('.png')).map((e) => e.path.slice(0, -4));
}

const say = (t) => { $('artmsg').textContent = t; };

/* One cover from the mirror, as a PNG blob. libretro stores a duplicate cover
 * as a symbolic link, and GitHub's raw files answer a link with its text: the
 * name of the file it points at, in the same folder. That text landed as four
 * "covers" on 2026-10-05, so a reply that is not a PNG is followed as a link,
 * a few hops at most, and never uploaded as it is. */
async function coverBlob(repo, name) {
	for (let hop = 0; hop < 4; hop++) {
		const r = await fetch(GH_RAW + repo + '/master/Named_Boxarts/' + enc(name) + '.png');
		if (!r.ok) throw new Error('GitHub answered ' + r.status);
		const blob = await r.blob();
		const head = new Uint8Array(await blob.slice(0, 4).arrayBuffer());

		if (head[0] === 0x89 && head[1] === 0x50 && head[2] === 0x4E && head[3] === 0x47)
			return blob;
		const link = (await blob.text()).trim();
		if (!link || link.length > 300 || link.includes('\n') || !link.endsWith('.png'))
			throw new Error('not a picture');
		name = link.replace(/^.*\//, '').slice(0, -4);
	}
	throw new Error('links lead nowhere');
}

/* libretro's file names are No-Intro's with these characters as _. */
const libretroName = (name) => name.replace(/[&*\/:`<>?\\|]/g, '_');

/* Games' covers, best-first (Eric, 2026-10-05), counted into `n`:
 *
 *   1. the game's own name, asked for directly - a file named the No-Intro way
 *      already is the cover's name
 *   2. its checksum's No-Intro name, from the console's No-Intro list, asked
 *      for directly
 *   3. only for what is left, the collection's listing, matched by name and
 *      then loosely
 *
 * The first two use only raw.githubusercontent.com, which has no hourly cap;
 * the listing is GitHub's API, 60 requests an hour, which runs out after a
 * few full runs. A miss by the game's own name is ordinary and not reported;
 * a cover the checksum or the listing named that then would not come or would
 * not go onto the card is. The checksum step's failures were silent at first,
 * and the mono Neo Geo Pocket's covers went missing with nothing to say why
 * (2026-10-05). */
async function getArt(n, fails, notes) {
	const w = await wanted('art');
	/* The listing's failure, once seen: the rest of the run goes on without
	 * it. GitHub's hour running out used to end the whole run, so a console
	 * after it never had the two ways that need no listing (2026-10-05, both
	 * Neo Geo Pockets). */
	let noList = '', waiting = 0;

	if (!w) return;
	for (const s of w.shelves) {
		const left = new Set(s.games);
		const cover = (stem) => 'roms/' + s.folder + '/.media/' + stem + '.png';

		/* The second collection (a console's disc games) only for what the
		 * first left. */
		for (const c of s.collections) {
			if (!left.size) break;
			const repo = repoOf(c);
			const put = async (stem, name, report) => {
				try {
					await api('PUT', '/api/file?p=' + enc(cover(stem)), await coverBlob(repo, name));
					n.games++;
					left.delete(stem);
				} catch (err) {
					if (err.message === 'unauthorized') throw err;
					/* 'some': all but a 404, which for a checksum's name only
					 * means libretro files that dump's cover under another name,
					 * and the listing finds it (three such, 2026-10-05). */
					if (report === true || (report === 'some' && !/ 404$/.test(err.message)))
						fails.push(stem + ' (' + err.message + ')');
				}
			};
			const each = async (pairs, how, report) => {
				let i = 0;
				for (const [stem, name] of pairs) {
					if (!left.has(stem)) continue;
					say(s.name + ': ' + how + ', ' + (++i) + ' of ' + pairs.length + ', ' + stem);
					await put(stem, name, report);
				}
			};

			await each([...left].map((stem) => [stem, libretroName(stem)]), 'by name', false);

			if (left.size) {
				say(s.name + ': looking games up by checksum');
				const dat = await fetch(DAT + enc(c) + '.dat').catch(() => null);
				if (dat && dat.ok) {
					const byCrc = await (await api('POST', '/api/art/nointro?s=' + enc(s.folder),
					                               await dat.text())).json();
					await each(byCrc.matches.map((m) => [m.stem, libretroName(m.name)]),
					           'by checksum', 'some');
				}
			}

			if (left.size && noList) waiting += left.size;
			if (left.size && !noList) {
				say(s.name + ': looking through libretro\'s list for ' + left.size + ' more');
				let names;
				try {
					names = await coverNames(repo);
				} catch (err) {
					noList = err.message;
					waiting += left.size;
					continue;
				}
				if (!names.length) continue;
				const byName = await (await api('POST', '/api/art/match?s=' + enc(s.folder),
				                                names.join('\n'))).json();
				await each(byName.matches.map((m) => [m.stem, m.name]), 'from the list', true);
				/* And loosely, last of all: the device's loose pass, given no
				 * checksum list ("-") since the checksums have had their turn. */
				if (left.size) {
					const loose = await (await api('POST', '/api/art/crc?s=' + enc(s.folder),
					                               '-')).json();
					await each(loose.matches.map((m) => [m.stem, m.name]), 'loosely', true);
				}
			}
		}
	}
	if (noList)
		notes.push(' ' + waiting + (waiting === 1 ? ' game waits' : ' games wait') +
		           ' for libretro\'s list: ' + noList);
}

/* Album covers: MusicBrainz names the release, the device picks it by Muse's
 * track-count rule (the first hit is often a single of the same name), and
 * the Cover Art Archive has its front at 500px. MusicBrainz asks for a request
 * a second at most, so searches are spaced out. */
const CAA = 'https://coverartarchive.org/release-group/';
let mbLast = 0;

async function musicbrainz(url) {
	const wait = mbLast + 1100 - Date.now();
	if (wait > 0) await new Promise((ok) => setTimeout(ok, wait));
	mbLast = Date.now();
	const r = await fetch(url, { headers: { Accept: 'application/json' } });
	if (r.status === 503) throw new Error('MusicBrainz is busy. Try again in a minute.');
	return r.ok ? r.text() : null;
}

async function getAlbums(n, fails) {
	const w = await wanted('albums');

	if (!w) return;
	for (let i = 0; i < w.albums.length; i++) {
		const al = w.albums[i];
		let pick = null;

		say('Albums: ' + (i + 1) + ' of ' + w.albums.length + ', ' + al.album);
		for (const url of al.searches) {
			const reply = await musicbrainz(url);
			if (!reply) continue;
			pick = await (await api('POST', '/api/albums/pick?id=' + al.id, reply)).json();
			if (pick.rg) break;
		}
		if (!pick || !pick.rg) continue;
		try {
			const img = await fetch(CAA + pick.rg + '/front-500');
			/* Known to MusicBrainz with no front cover given: not found. */
			if (img.status === 404) continue;
			if (!img.ok) throw new Error('the Cover Art Archive answered ' + img.status);
			await api('PUT', '/api/file?p=' + enc(pick.cover), await img.blob());
			n.albums++;
		} catch (err) {
			if (err.message === 'unauthorized') throw err;
			fails.push(al.album + ' (' + err.message + ')');
		}
	}
}

/* One button, games and then albums. */
async function getCovers() {
	const n = { games: 0, albums: 0 };
	const fails = [], notes = [];
	const count = (k, one, many) => n[k] + ' ' + (n[k] === 1 ? one : many);

	artBusy = true;
	$('artgo').disabled = true;
	try {
		await getArt(n, fails, notes);
		await getAlbums(n, fails);
		toast('Covers for ' + count('games', 'game', 'games') + ' and ' +
		      count('albums', 'album', 'albums') +
		      (fails.length ? ', ' + fails.length + ' failed' : ''),
		      !n.games && !n.albums && fails.length > 0);
		lastRun = failNote(fails) + notes.join('');
	} catch (err) {
		if (err.message !== 'unauthorized') toast(err.message, true);
		lastRun = ' Last time it stopped: ' + err.message + failNote(fails);
	} finally {
		artBusy = false;
		go(cwd, 'none').catch(() => {});
	}
}

$('artgo').addEventListener('click', () => { if (!artBusy) getCovers(); });

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
		await start();
	} catch (e) {
		showGate('');
	}
})();
