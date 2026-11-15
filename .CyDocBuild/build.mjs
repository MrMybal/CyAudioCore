import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const workspace = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.some(arg => arg !== '--check')) throw new Error('Usage: node .CyDocBuild/build.mjs [--check]');
if (Number(process.versions.node.split('.')[0]) < 22) throw new Error('Node.js 22 or later is required');
const engine = resolve(workspace, 'internal/build/node_modules/cydoc');
const pin = JSON.parse(await readFile(resolve(workspace, 'toolchain.json'), 'utf8'));
const metadata = JSON.parse(await readFile(resolve(engine, 'package.json'), 'utf8'));
if (metadata.name !== 'cydoc' || metadata.version !== pin.version) throw new Error('Incorrect CyDoc package');
for (const name of ['marked', 'prismjs', 'esbuild']) {
  const dependency = JSON.parse(await readFile(resolve(engine, '..', name, 'package.json'), 'utf8'));
  if (dependency.version !== pin.buildDependencies[name]) throw new Error(`Incorrect generation dependency: ${name}`);
}
for (const [name, record] of Object.entries(pin.files)) {
  const actual = createHash('sha256').update(await readFile(resolve(engine, name))).digest('hex');
  if (actual !== record.sha256) throw new Error(`CyDoc file differs from pinned generator: ${name}`);
}
for (const [name, asset] of Object.entries(pin.readerAssets)) {
  const actual = createHash('sha256').update(await readFile(resolve(workspace, 'assets', name))).digest('hex');
  if (actual !== asset.sha256) throw new Error(`Reader asset differs from pinned source: ${name}`);
}
const child = spawn(process.execPath, [resolve(engine, 'tools/cli.mjs'), args.includes('--check') ? 'check' : 'build',
  resolve(workspace, 'cydoc.config.json')], { cwd: workspace, stdio: 'inherit', windowsHide: true });
child.on('error', error => { console.error(error.message); process.exitCode = 1; });
child.on('exit', code => { process.exitCode = code ?? 1; });
