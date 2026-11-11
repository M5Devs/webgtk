#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Drive a ninja engine build to completion by repairing what it trips over.

`scripts/gen_stub_driver.py` does this for the *gn gen* phase: it stubs the GN
files the pruned archive drops until `gn gen` parses. This does the same for
the *compile* phase. Ninja stops at the first thing it cannot resolve; the
failures seen on a pruned Chromium tree are all mechanical:

  * a source file under a git-dependency path is absent because the archive
    ships that dependency as an empty ``src/``. The revision is pinned in
    ``DEPS``, so the dependency can be fetched back at the exact commit the
    tree was pruned from.
  * a tree provisioned from Google Cloud Storage (the Node.js binaries and
    their ``node_modules``) is absent. ``DEPS`` names the bucket and object, so
    it can be downloaded and unpacked.
  * a CIPD-provisioned tool (``buildtools/third_party/mold/cipd/mold``) is
    absent. ``DEPS`` names the CIPD package and instance, so it can be
    installed.
  * a *generated* header (``gen/...``) is absent because the target that emits
    it has not run. Ninja can build it by path.
  * a file in a vendored Rust crate is absent. The crate is published on
    crates.io at the name and version the vendored ``Cargo.toml`` records.
  * a file that belongs to the Chromium tree itself (``//testing``, ``//tools``)
    is absent. The tree's own release tag names it on chromium.googlesource.com.
  * a system library a BUILD.gn probes with pkg-config is absent. That is a
    host-environment problem, not a tree problem, and is reported, not fixed.

Each repair is recorded in a checkpoint file, so an interrupted run resumes
without repeating work. The loop is deliberately conservative: it only acts on
failures it can classify, and it stops and reports on anything else rather than
guessing.

    python3 scripts/engine_autoheal.py [engine-root] [options]

Run it after ``scripts/setup_engine.sh`` has produced ``out/webgtk``.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

# --------------------------------------------------------------- DEPS parsing
#
# DEPS is GN-flavoured Python. Only a small subset matters here: the string
# variables and the git/CIPD dependency entries, whose values are built by
# concatenating literals with Var('name') lookups. A full parser would be
# overkill; this evaluates exactly that shape.

VAR_RE = re.compile(r"Var\(\s*'([^']+)'\s*\)")
# A quoted literal, so `'a' + Var('x') + '/b.git'` splits into pieces.
PIECE_RE = re.compile(r"'([^']*)'|Var\(\s*'([^']+)'\s*\)")


def _blocks(text, name):
    """Yield the text of each ``name = { ... }`` block, brace-balanced."""
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*=\s*\{', text):
        depth = 0
        i = m.end() - 1
        start = i
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    yield text[start:i + 1]
                    break
            i += 1


def parse_vars(text):
    """Collect the flat string variables declared in any ``vars = {...}``."""
    out = {}
    for block in _blocks(text, 'vars'):
        for m in re.finditer(r"'([A-Za-z0-9_]+)'\s*:\s*'([^']*)'", block):
            out[m.group(1)] = m.group(2)
    return out


def eval_expr(expr, variables):
    """Evaluate a DEPS string expression, or return None if it is not one.

    Handles ``Var('x')``, quoted literals and ``+`` concatenation. A dep whose
    value is a function call or a conditional resolves to None and is skipped
    by the caller.
    """
    pieces = []
    for lit, var in PIECE_RE.findall(expr):
        if var:
            if var not in variables:
                return None
            pieces.append(variables[var])
        else:
            pieces.append(lit)
    if not pieces:
        return None
    joined = ''.join(pieces)
    return joined or None


def iter_dep_entries(block):
    """Yield (rel, value_text) for each ``'src/<rel>': <value>`` in a deps block.

    ``value_text`` is the balanced ``{...}`` for dict-form entries, or the
    expression text up to the depth-0 comma for ``url + '@' + rev`` entries.
    """
    for m in re.finditer(r"'src/([^']+)'\s*:\s*", block):
        rel = m.group(1).rstrip('/')
        i = m.end()
        while i < len(block) and block[i] in ' \t':
            i += 1
        if i < len(block) and block[i] == '{':
            depth = 0
            j = i
            while j < len(block):
                if block[j] == '{':
                    depth += 1
                elif block[j] == '}':
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            yield rel, block[i:j]
        else:
            depth = 0
            in_str = False
            j = i
            while j < len(block):
                ch = block[j]
                if ch == "'":
                    in_str = not in_str
                elif not in_str:
                    if ch in '([{':
                        depth += 1
                    elif ch in ')]}':
                        depth -= 1
                    elif ch == ',' and depth == 0:
                        break
                j += 1
            yield rel, block[i:j].strip()


def parse_git_deps(text, variables):
    """Map 'third_party/foo/src' -> {'url': ..., 'rev': ...} from DEPS.

    Handles both the plain ``url + '@' + rev`` form and the dict form whose
    ``'url'`` key holds the same expression. CIPD, GCS and hook entries are
    skipped here.
    """
    deps = {}
    for block in _blocks(text, 'deps'):
        for rel, value in iter_dep_entries(block):
            if not value:
                continue
            expr = value
            if value.startswith('{'):
                if "'url'" not in value:
                    continue
                um = re.search(r"'url'\s*:\s*", value)
                after = value[um.end():]
                depth = 0
                in_str = False
                cut = len(after)
                for i, ch in enumerate(after):
                    if ch == "'":
                        in_str = not in_str
                    elif not in_str:
                        if ch in '([{':
                            depth += 1
                        elif ch in ')]}':
                            depth -= 1
                        elif ch == ',' and depth == 0:
                            cut = i
                            break
                expr = after[:cut].strip()
            resolved = eval_expr(expr, variables)
            if not resolved or '@' not in resolved:
                continue
            url, _, rev = resolved.rpartition('@')
            deps[rel] = {'url': url.strip(), 'rev': rev.strip()}
    return deps


def parse_cipd_deps(text, variables):
    """Map 'buildtools/.../cipd' -> [(package, version), ...] from DEPS."""
    out = {}
    for block in _blocks(text, 'deps'):
        for rel, value in iter_dep_entries(block):
            if not value.startswith('{') or "'cipd'" not in value:
                continue
            packages = []
            for pm in re.finditer(
                    r"'package'\s*:\s*'([^']+)'\s*,\s*"
                    r"'version'\s*:\s*'([^']+)'", value):
                packages.append((pm.group(1), pm.group(2)))
            if packages:
                out[rel] = packages
    return out


def parse_gcs_deps(text, variables):
    """Map 'third_party/node/node_modules' -> [(bucket, object, out, sha256)].

    Chromium pulls several trees from Google Cloud Storage (the Node.js
    binaries and their node_modules, the clang and Rust toolchains, the sysroot)
    via ``dep_type: gcs``. On a pruned tree those are simply absent, and the
    bucket plus object name in DEPS are enough to fetch them back.
    """
    out = {}
    for block in _blocks(text, 'deps'):
        for rel, value in iter_dep_entries(block):
            if not value.startswith('{') or "'gcs'" not in value:
                continue
            bm = re.search(r"'bucket'\s*:\s*'([^']+)'", value)
            if not bm:
                continue
            bucket = bm.group(1)
            objects = []
            for om in re.finditer(r"'object_name'\s*:\s*'([^']+)'", value):
                # Each object's other keys follow it, before the next object.
                tail = value[om.end():]
                nxt = tail.find("'object_name'")
                seg = tail if nxt < 0 else tail[:nxt]
                outm = re.search(r"'output_file'\s*:\s*'([^']+)'", seg)
                shm = re.search(r"'sha256sum'\s*:\s*'([^']+)'", seg)
                objects.append((
                    bucket,
                    om.group(1),
                    outm.group(1) if outm else os.path.basename(om.group(1)),
                    shm.group(1) if shm else None,
                ))
            if objects:
                out[rel] = objects
    return out


# --------------------------------------------------------------- ninja output

# `ninja: error: '<path>', needed by '<target>', missing and no known rule`
MISSING_RE = re.compile(
    r"ninja: error: '([^']+)', needed by '([^']+)', "
    r"missing and no known rule to make it")
# A compile diagnostic, for the "cannot fix mechanically" path.
DIAG_RE = re.compile(r'^(.*?):(\d+):(\d+): (error|fatal error): (.*)$', re.M)
PKG_RE = re.compile(r"Package '([^']+)', required by '([^']+)', not found")


class Fix(object):
    def __init__(self, kind, detail):
        self.kind = kind
        self.detail = detail

    def __repr__(self):
        return '%s: %s' % (self.kind, self.detail)


def normalize_missing(root, path):
    """Turn a ninja-reported path into a tree-relative one, or None."""
    if path.startswith('../../'):
        return path[6:]
    if path.startswith('//'):
        return path[2:]
    if os.path.isabs(path):
        return os.path.relpath(path, root)
    return path


def best_dep(git_deps, cipd_deps, gcs_deps, rel):
    """Pick the most specific dependency that contains `rel`.

    Dependency paths nest: ``third_party/gperf`` is a git dependency while
    ``third_party/gperf/cipd`` inside it is a CIPD one. The file that is missing
    belongs to whichever path is the longest matching prefix, so that is the one
    to restore; checking the dep kinds in a fixed order would pick the wrong
    (shorter) one.
    """
    best = None
    for kind, table in (('git', git_deps), ('cipd', cipd_deps),
                        ('gcs', gcs_deps)):
        for dep_path, value in table.items():
            if rel == dep_path or rel.startswith(dep_path + '/'):
                if best is None or len(dep_path) > len(best[1]):
                    best = (kind, dep_path, value)
    return best


# --------------------------------------------------------------- repair steps

def fetch_git_dep(root, rel_dep, info, log):
    """Fetch a pruned git dependency at its pinned revision."""
    dest = os.path.join(root, rel_dep)
    # A pruned checkout is a directory with an empty (or absent) src payload;
    # clone into a temp dir and swap in, so a partial fetch never leaves a
    # half-populated tree.
    tmp = dest.rstrip('/') + '.autoheal'
    subprocess.run(['rm', '-rf', tmp], check=False)
    os.makedirs(tmp, exist_ok=True)
    steps = [
        ['git', 'init', '-q', tmp],
        ['git', '-C', tmp, 'remote', 'add', 'origin', info['url']],
        ['git', '-C', tmp, 'fetch', '-q', '--depth', '1',
         'origin', info['rev']],
        ['git', '-C', tmp, 'checkout', '-q', 'FETCH_HEAD'],
    ]
    for cmd in steps:
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            log('  fetch failed: %s\n%s' % (' '.join(cmd), proc.stderr.strip()))
            subprocess.run(['rm', '-rf', tmp], check=False)
            return False
    subprocess.run(['rm', '-rf', os.path.join(tmp, '.git')], check=False)
    # Preserve any files the archive kept alongside the checkout (BUILD.gn,
    # OWNERS, ...) by merging rather than replacing.
    os.makedirs(dest, exist_ok=True)
    subprocess.run(['cp', '-a', tmp + '/.', dest + '/'], check=False)
    subprocess.run(['rm', '-rf', tmp], check=False)
    log('  restored %s at %s' % (rel_dep, info['rev'][:12]))
    return True


def cipd_platform():
    """Return the CIPD '${{platform}}' value for the host, e.g. linux-amd64."""
    machine = os.uname().machine
    arch = {
        'x86_64': 'amd64', 'amd64': 'amd64',
        'aarch64': 'arm64', 'arm64': 'arm64',
        'i686': '386', 'i386': '386',
    }.get(machine, machine)
    osname = {'Linux': 'linux', 'Darwin': 'mac', 'Windows': 'windows'}.get(
        os.uname().sysname, 'linux')
    return '%s-%s' % (osname, arch)


def install_cipd_dep(root, engine_dir, rel_dep, packages, log):
    """Install the CIPD packages DEPS pins under a tree path."""
    cipd = os.path.join(engine_dir, 'cipd')
    if not os.path.exists(cipd):
        log('  cipd client missing at %s' % cipd)
        return False
    root_dir = os.path.join(root, rel_dep)
    os.makedirs(root_dir, exist_ok=True)
    ok = True
    for package, version in packages:
        package = package.replace('${{platform}}', cipd_platform())
        proc = subprocess.run(
            [cipd, 'install', '--root', root_dir, package, version],
            capture_output=True, text=True)
        if proc.returncode != 0:
            log('  cipd install %s failed:\n%s' % (package, proc.stderr.strip()))
            ok = False
        else:
            log('  installed CIPD %s' % package)
    return ok


def host_gcs_labels():
    """The object-name prefixes in chromium-browser-clang for this host.

    The clang and Rust toolchain entries list one object per platform under a
    prefix such as ``Linux_x64/``. gclient downloads only the host's; picking
    all of them would extract three toolchains over each other.
    """
    machine = os.uname().machine
    if sys.platform == 'darwin':
        return ['Mac_arm64'] if machine == 'arm64' else ['Mac']
    if os.name == 'nt':
        return ['Win']
    if machine in ('x86_64', 'amd64'):
        return ['Linux_x64']
    return ['Linux_Arm64', 'Linux_arm64', 'Linux_Armv7']


def fetch_gcs_dep(root, rel_dep, objects, log):
    """Download and unpack the GCS objects DEPS pins under a tree path.

    Chromium stores the Node.js binaries and their node_modules (and the clang
    and Rust toolchains, and the sysroot) as ``dep_type: gcs`` tarballs. Each
    ``output_file`` is extracted into the dependency directory, which is what
    gclient does. Where the entry lists a per-platform object, only the host's
    is used.
    """
    prefixed = [o for o in objects if '/' in o[1]]
    if prefixed:
        labels = set(host_gcs_labels())
        chosen = [o for o in objects if o[1].split('/')[0] in labels]
        if chosen:
            objects = chosen
        else:
            single = [o for o in objects if '/' not in o[1]]
            if single:
                objects = single
            else:
                log('  no GCS object for host %s in %s (have %s)'
                    % (sorted(labels)[0], rel_dep,
                       sorted({o[1].split('/')[0] for o in prefixed})))
                return False
    dest = os.path.join(root, rel_dep)
    os.makedirs(dest, exist_ok=True)
    for bucket, obj, out_file, sha256 in objects:
        url = 'https://storage.googleapis.com/%s/%s' % (bucket, obj)
        tmp = os.path.join(dest, out_file)
        proc = subprocess.run(
            ['curl', '-fsSL', '-o', tmp, url], capture_output=True, text=True)
        if proc.returncode != 0:
            log('  download failed: %s\n%s' % (url, proc.stderr.strip()[:300]))
            return False
        if sha256:
            got = hashlib.sha256(open(tmp, 'rb').read()).hexdigest()
            if got != sha256:
                log('  checksum mismatch for %s (want %s, got %s)'
                    % (out_file, sha256[:12], got[:12]))
                os.remove(tmp)
                return False
        if out_file.endswith(('.tar.gz', '.tgz', '.tar.xz', '.tar.bz2')):
            untar = subprocess.run(
                ['tar', '-xf', tmp, '-C', dest, '--overwrite', '-m'],
                capture_output=True, text=True)
            if untar.returncode != 0:
                log('  extract failed for %s:\n%s'
                    % (out_file, untar.stderr.strip()[:300]))
                return False
            os.remove(tmp)
        log('  fetched gs://%s/%s -> %s' % (bucket, obj, rel_dep))
    return True


def chromium_version_tag(root):
    """Derive the Chromium release tag from chrome/VERSION.

    The pruned archive is a snapshot of one Chromium release, and it records
    that release in chrome/VERSION (for example 143.0.7499.0). The same string
    is a git tag on chromium.googlesource.com, so a file the archive dropped can
    be fetched back at the exact revision the tree came from.
    """
    path = os.path.join(root, 'chrome', 'VERSION')
    if not os.path.isfile(path):
        return None
    values = {}
    with open(path) as f:
        for line in f:
            if '=' in line:
                key, _, val = line.strip().partition('=')
                values[key.strip()] = val.strip()
    parts = [values.get(k) for k in ('MAJOR', 'MINOR', 'BUILD', 'PATCH')]
    if not all(parts):
        return None
    return '.'.join(parts)


def mirror_refs(root, tag):
    """Candidate refs to try on chromium.googlesource.com, best first.

    The archive is pruned from one Chromium revision and records its release in
    chrome/VERSION, which is usually the exact tag. A few files are newer than
    that tag (the snapshot can sit between releases), so the branch head for the
    same build number and the main branch are tried as fallbacks. A ref that
    serves the file wins; if none does, the caller reports and stops.
    """
    refs = []
    if tag:
        refs.append('refs/tags/%s' % tag)
        build = tag.split('.')[2] if tag.count('.') >= 2 else None
        if build:
            refs.append('refs/branch-heads/%s' % build)
    refs.append('main')
    return refs


def _safe_extract(tar, dest, members):
    """Extract selected members, using tarfile's safe filter when available."""
    import inspect
    import tarfile
    if 'filter' in inspect.signature(tarfile.TarFile.extractall).parameters:
        tar.extractall(path=dest, members=members, filter='data')
    else:
        tar.extractall(path=dest, members=members)


def _download(url, log, rel, tag, allow_404=False):
    """GET a URL with retry/backoff. Returns bytes, or None on 404/give-up."""
    import urllib.request
    import urllib.error
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as resp:
                return resp.read()
        except urllib.error.HTTPError as exc:
            if exc.code == 404 and allow_404:
                return None
            if exc.code == 404:
                log('  upstream has no %s at %s' % (rel, tag))
                return None
            log('  transient HTTP %s for %s (attempt %d)'
                % (exc.code, rel, attempt + 1))
            time.sleep(2 * (attempt + 1))
        except Exception as exc:  # noqa: BLE001 - report any transport failure
            log('  fetch of %s failed (attempt %d): %s'
                % (rel, attempt + 1, exc))
            time.sleep(2 * (attempt + 1))
    return None


def fetch_source_dir(root, rel_dir, tag, log, refs):
    """Restore a whole dropped directory from the tree's googlesource refs.

    googlesource serves a directory as a tarball via ``+archive``. A pruned
    archive often drops a directory wholesale (a vendored crate, a data
    folder), so restoring the directory that holds the missing file recovers it
    and its siblings in one request instead of one request per file. Existing
    files are left alone, so the fetch only fills gaps.
    """
    import io
    import tarfile
    for ref in refs:
        url = ('https://chromium.googlesource.com/chromium/src/+archive/%s/%s'
               '.tar.gz' % (ref, rel_dir))
        data = _download(url, log, rel_dir, ref, allow_404=True)
        if data is None:
            continue
        dest = os.path.join(root, rel_dir)
        os.makedirs(dest, exist_ok=True)
        try:
            with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
                members = []
                for member in tar.getmembers():
                    target = os.path.join(dest, member.name)
                    # Never let an archive entry escape the destination.
                    if os.path.commonpath([os.path.abspath(dest),
                                           os.path.abspath(target)]) != \
                            os.path.abspath(dest):
                        continue
                    if os.path.exists(target):
                        continue
                    members.append(member)
                _safe_extract(tar, dest, members)
        except Exception as exc:  # noqa: BLE001
            log('  archive extract failed for %s: %s' % (rel_dir, exc))
            continue
        log('  restored directory %s from chromium/%s' % (rel_dir, ref))
        return True
    return False


def fetch_source_file(root, rel, tag, log, refs):
    """Fetch one dropped source-tree file from chromium.googlesource.com.

    Used for files that belong to the Chromium tree itself (//testing, //tools,
    //components data) rather than to a git/CIPD/GCS dependency, and so are not
    restorable from DEPS.
    """
    import base64
    for ref in refs:
        url = ('https://chromium.googlesource.com/chromium/src/+/%s/%s'
               '?format=TEXT' % (ref, rel))
        data = _download(url, log, rel, ref)
        if data is None:
            continue
        try:
            payload = base64.b64decode(data, validate=False)
        except Exception:  # noqa: BLE001
            payload = data
        dest = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'wb') as f:
            f.write(payload)
        log('  restored %s from chromium/%s' % (rel, ref))
        return True
    return False


def parse_cargo_toml(path):
    """Return (name, version) from a Cargo.toml, or (None, None)."""
    name = version = None
    if not os.path.isfile(path):
        return None, None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.strip()
            if line.startswith('name =') and name is None:
                name = line.split('=', 1)[1].strip().strip('"')
            elif line.startswith('version =') and version is None:
                version = line.split('=', 1)[1].strip().strip('"')
            if name and version:
                break
    return name, version


def fetch_rust_crate(root, rel, log):
    """Restore a file in a vendored Rust crate from crates.io.

    Chromium vendors its Rust crates under
    ``third_party/rust/chromium_crates_io/vendor``; a pruned archive drops some
    of their files, and the crates are not part of the Chromium tree on
    googlesource, so the version-tag mirror cannot serve them. The crate is
    published on crates.io at the exact name and version the vendored
    Cargo.toml records, so the file can be pulled from the crate tarball.
    Missing files are filled in; existing ones (including Chromium's own edits)
    are left alone.
    """
    import io
    import tarfile
    prefix = 'third_party/rust/chromium_crates_io/vendor/'
    if not rel.startswith(prefix):
        return False
    crate_dir, _, sub = rel[len(prefix):].partition('/')
    if not sub:
        return False
    vendor = os.path.join(root, prefix, crate_dir)
    name, version = parse_cargo_toml(os.path.join(vendor, 'Cargo.toml'))
    if not name or not version:
        log('  no Cargo.toml name/version for %s' % crate_dir)
        return False
    url = 'https://static.crates.io/crates/%s/%s-%s.crate' % (
        name, name, version)
    data = _download(url, log, rel, 'crates.io')
    if data is None:
        return False
    root_in_tar = '%s-%s' % (name, version)
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tar:
            members = []
            for member in tar.getmembers():
                if not member.name.startswith(root_in_tar + '/'):
                    continue
                inner = member.name[len(root_in_tar) + 1:]
                if not inner:
                    continue
                target = os.path.join(vendor, inner)
                if os.path.exists(target):
                    continue
                members.append((member, target))
            for member, target in members:
                member.name = os.path.relpath(target, vendor)
                os.makedirs(os.path.dirname(target), exist_ok=True)
            _safe_extract(tar, vendor, [m for m, _ in members])
    except Exception as exc:  # noqa: BLE001
        log('  crate extract failed for %s: %s' % (crate_dir, exc))
        return False
    log('  restored %s from %s %s (crates.io)' % (rel, name, version))
    return True


def restore_from_mirror(root, rel, tag, log):
    """Fill a gap in the Chromium tree itself, from the version-tag mirror.

    Prefer the enclosing directory (one request restores the siblings too),
    falling back to the single file when the directory archive is unavailable.
    """
    refs = mirror_refs(root, tag)
    parent = os.path.dirname(rel)
    if parent and fetch_source_dir(root, parent, tag, log, refs):
        if os.path.exists(os.path.join(root, rel)):
            return True
    return fetch_source_file(root, rel, tag, log, refs)


def build_generated(out_dir, rel, ninja, log):
    """Ask ninja to build a generated header by path."""
    proc = subprocess.run(
        [ninja, '-C', out_dir, rel], capture_output=True, text=True)
    if proc.returncode == 0:
        log('  generated %s' % rel)
        return True
    log('  ninja could not generate %s:\n%s' % (rel, proc.stderr.strip()[:400]))
    return False


# ------------------------------------------------------------------ main loop

def run_ninja(ninja, out_dir, target, jobs, log_path):
    cmd = [ninja, '-C', out_dir, '-k', '0', '-j', str(jobs), target]
    with open(log_path, 'w') as handle:
        proc = subprocess.run(cmd, stdout=handle, stderr=subprocess.STDOUT)
    return proc.returncode


def progress_line(text):
    found = re.findall(r'\[\s*(\d+)/(\d+)\]', text)
    if not found:
        return None
    done, total = found[-1]
    return '%s/%s' % (done, total)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('engine_root', nargs='?', default='engine',
                    help='engine root (holds chromium/ and gnroot/)')
    ap.add_argument('--out', default='out/webgtk', help='gn output directory')
    ap.add_argument('--target', default='content', help='ninja target')
    ap.add_argument('--jobs', default=str(os.cpu_count() or 4))
    ap.add_argument('--checkpoint', default='engine/autoheal_state.json',
                    help='where to record applied fixes')
    ap.add_argument('--max-rounds', type=int, default=2000)
    ap.add_argument('--max-fetches', type=int, default=200,
                    help='cap on dependency fetches per run')
    ap.add_argument('--prefetch', action='append', default=[], metavar='DEP',
                    help='fetch these DEPS dependency paths and exit '
                         '(repeatable); used by scripts/setup_toolchain.sh')
    ap.add_argument('--ninja', default=os.environ.get('NINJA', 'ninja'))
    args = ap.parse_args()

    engine_root = os.path.abspath(args.engine_root)
    root = os.path.join(engine_root, 'chromium')
    out_dir = os.path.join(root, args.out)
    ninja = args.ninja
    log_path = os.path.join(engine_root, 'autoheal_ninja.log')

    def log(msg):
        print(msg, flush=True)

    if not os.path.isfile(os.path.join(root, 'DEPS')):
        log('error: %s is not a Chromium source root (no DEPS)' % root)
        return 2

    with open(os.path.join(root, 'DEPS')) as f:
        deps_text = f.read()
    variables = parse_vars(deps_text)
    git_deps = parse_git_deps(deps_text, variables)
    cipd_deps = parse_cipd_deps(deps_text, variables)
    gcs_deps = parse_gcs_deps(deps_text, variables)
    log('DEPS: %d git, %d CIPD, %d GCS dependencies'
        % (len(git_deps), len(cipd_deps), len(gcs_deps)))
    version_tag = chromium_version_tag(root)
    if version_tag:
        log('tree version tag: %s' % version_tag)

    if args.prefetch:
        failures = 0
        for want in args.prefetch:
            want = want.rstrip('/')
            dep = best_dep(git_deps, cipd_deps, gcs_deps, want)
            if not dep or dep[1] != want:
                log('prefetch: %s is not an exact DEPS dependency path' % want)
                failures += 1
                continue
            kind, dep_path, value = dep
            log('prefetch %s (%s)' % (dep_path, kind))
            if kind == 'git':
                ok = fetch_git_dep(root, dep_path, value, log)
            elif kind == 'cipd':
                ok = install_cipd_dep(root, engine_root, dep_path, value, log)
            else:
                ok = fetch_gcs_dep(root, dep_path, value, log)
            failures += 0 if ok else 1
        return 0 if failures == 0 else 1

    if not os.path.isfile(os.path.join(out_dir, 'build.ninja')):
        log('error: no gn output at %s; run scripts/setup_engine.sh first'
            % out_dir)
        return 2

    state = {'applied': [], 'fetches': 0, 'rounds': 0}
    if os.path.exists(args.checkpoint):
        with open(args.checkpoint) as f:
            state = json.load(f)
        log('resuming: %d fixes already applied' % len(state['applied']))
    applied = set(tuple(x) for x in state['applied'])

    def save():
        state['applied'] = [list(x) for x in applied]
        with open(args.checkpoint, 'w') as f:
            json.dump(state, f, indent=1)

    started = time.time()
    for rnd in range(1, args.max_rounds + 1):
        state['rounds'] = rnd
        log('\n==> round %d: ninja -C %s %s' % (rnd, args.out, args.target))
        rc = run_ninja(ninja, out_dir, args.target, args.jobs, log_path)
        with open(log_path, errors='replace') as f:
            text = f.read()
        if rc == 0:
            log('==> build succeeded in %d rounds (%.1f min)'
                % (rnd, (time.time() - started) / 60.0))
            save()
            return 0

        prog = progress_line(text)
        if prog:
            log('    progress: [%s]' % prog)

        m = MISSING_RE.search(text)
        if not m:
            # Not a missing-input error. Report the real diagnostics so a human
            # can decide; the driver never guesses at a compile failure.
            pkg = PKG_RE.search(text)
            if pkg:
                log('==> stopped: host package missing: %s (needed by %s)'
                    % (pkg.group(1), pkg.group(2)))
                log('    run scripts/setup_gn_env.sh, then re-run.')
                save()
                return 3
            diags = DIAG_RE.findall(text)
            if diags:
                log('==> stopped on a compile error (not mechanically fixable):')
                for d in diags[:15]:
                    log('    %s:%s: %s: %s' % (d[0], d[1], d[3], d[4]))
                save()
                return 4
            log('==> stopped on an unclassified ninja error:')
            log(text[-1500:])
            save()
            return 1

        missing, needed_by = m.group(1), m.group(2)
        rel = normalize_missing(root, missing)
        key = ('missing', rel)
        log('    missing: %s (needed by %s)' % (rel, needed_by))

        if key in applied:
            log('==> already repaired %s once and it is still missing; '
                'stopping to avoid a loop' % rel)
            save()
            return 5

        # 1. the most specific DEPS dependency that contains the missing path:
        #    a git checkout pruned to an empty src/, a CIPD package, or a GCS
        #    object tree.
        dep = best_dep(git_deps, cipd_deps, gcs_deps, rel)
        if dep:
            kind, dep_path, value = dep
            if state['fetches'] >= args.max_fetches:
                log('==> fetch cap (%d) reached' % args.max_fetches)
                save()
                return 6
            log('    -> %s dependency %s' % (kind, dep_path))
            if kind == 'git':
                ok = fetch_git_dep(root, dep_path, value, log)
            elif kind == 'cipd':
                ok = install_cipd_dep(root, engine_root, dep_path, value, log)
            else:
                ok = fetch_gcs_dep(root, dep_path, value, log)
            if ok:
                applied.add(key)
                state['fetches'] += 1
                save()
                continue
            save()
            return 7

        # 2. a generated file ninja can build by path
        if rel.startswith('gen/') or rel.endswith('.h'):
            if build_generated(out_dir, rel, ninja, log):
                applied.add(key)
                save()
                continue

        # 3. a file in a vendored Rust crate: the crate is on crates.io at the
        #    name and version its Cargo.toml records.
        if rel.startswith('third_party/rust/chromium_crates_io/vendor/'):
            if fetch_rust_crate(root, rel, log):
                applied.add(key)
                state['fetches'] += 1
                save()
                continue

        # 4. a file that belongs to the Chromium tree itself (//testing,
        #    //tools, //components data) and is therefore not in DEPS: restore
        #    it from the googlesource mirror at the tree's own version tag.
        if version_tag and not rel.startswith('gen/'):
            if restore_from_mirror(root, rel, version_tag, log):
                applied.add(key)
                state['fetches'] += 1
                save()
                continue

        log('==> cannot classify %s; stopping' % rel)
        save()
        return 9

    log('==> reached the round cap without finishing')
    save()
    return 10


if __name__ == '__main__':
    sys.exit(main())
