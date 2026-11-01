#!/bin/bash
# A real pacman transaction, in a throwaway root: old local compositor and
# lib-old become new local compositor + lib-new in ONE transaction, with a
# snapshot DB and file repository. No live pacman DB or system files change.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d /var/tmp/locachy-update-pacman.XXXXXX)
trap 'rm -rf "$out"' EXIT
export src out
python3 -B - <<'PY'
import gzip,hashlib,importlib.util,io,json,os,subprocess,tarfile
from pathlib import Path
out=Path(os.environ['out']);src=Path(os.environ['src']);root=out/'root';cache=out/'cache'
(root/'var/lib/pacman/local').mkdir(parents=True);(root/'var/lib/pacman/sync').mkdir();cache.mkdir()
(root/'tmp').mkdir();(root/'no-hooks').mkdir()
root_db=root/'var/lib/pacman'
config=out/'base.conf'
config.write_text('[options]\nArchitecture = x86_64\nSigLevel = Never\nLocalFileSigLevel = Never\n[extra]\nServer = '+cache.as_uri()+'\n')
def archive(name,version,depend=()):
    path=cache/(name+'-'+version+'-any.pkg.tar.gz')
    text='pkgname = '+name+'\npkgver = '+version+'\npkgdesc = test package\nurl = https://example.invalid\nbuilddate = 1\npackager = test\nsize = 1\narch = any\nlicense = MIT\n'
    for d in depend:text+='depend = '+d+'\n'
    with tarfile.open(path,'w:gz') as t:
        data=text.encode();i=tarfile.TarInfo('.PKGINFO');i.size=len(data);t.addfile(i,io.BytesIO(data))
        # As makepkg makes them: pacman extracts .MTREE into the local DB entry,
        # which is what a symlinked alternate local refused.
        data=gzip.compress(b'#mtree\n');i=tarfile.TarInfo('.MTREE');i.size=len(data);t.addfile(i,io.BytesIO(data))
        data=(name+' '+version+'\n').encode();i=tarfile.TarInfo('usr/share/locachy-fixture/'+name);i.size=len(data);i.mode=0o644;t.addfile(i,io.BytesIO(data))
    return path
oldlib=archive('fixture-lib-old','1-1');old=archive('fixture-compositor','1-1',['fixture-lib-old'])
oldtools=archive('fixture-tools','1-1',['fixture-lib-old'])
newlib=archive('fixture-lib-new','2-1');new=archive('fixture-compositor','2-1',['fixture-lib-new'])
newtools=archive('fixture-tools','2-1',['fixture-lib-new'])
def pacman(args):
    subprocess.run(['pacman','--root',str(root),'--dbpath',str(root_db),'--config',str(config),
        '--cachedir',str(cache),'--hookdir',str(root/'no-hooks'),'--noconfirm']+args,check=True)
pacman(['-U',str(oldlib),str(old),str(oldtools)])
subprocess.run(['repo-add',str(cache/'extra.db.tar.gz'),str(newlib)],check=True,stdout=subprocess.DEVNULL)
import shutil
shutil.copyfile(cache/'extra.db.tar.gz',root_db/'sync/extra.db')
recipe=out/'recipe';recipe.mkdir();(recipe/'PKGBUILD').write_text('fixture only\n')
s=importlib.util.spec_from_file_location('tx',src/'link/locachy_transaction.py');tx=importlib.util.module_from_spec(s);s.loader.exec_module(tx)
names=('fixture-compositor','fixture-tools')
ref=tx.snapshot(out/'state',root_db/'sync',config,'fixture-host',
    {name:{'baseline_dir':str(recipe),'recipe_dir':str(recipe)} for name in names})
d,data=tx.load_snapshot(ref,'fixture-host');results={}
artifacts={name:tx.file_hash(package) for name,package in zip(names,(new,newtools))}
for name,package in zip(names,(new,newtools)):
    p=d/'prepared'/name;p.mkdir(parents=True)
    shutil.copyfile(package,p/package.name);artifact=p/package.name
    (p/'target-base-packages.txt').write_text('fixture-lib-old 1-1\n')
    (p/'target-packages.txt').write_text('fixture-compositor 2-1\nfixture-lib-new 2-1\nfixture-lib-old 1-1\nfixture-tools 2-1\n')
    r={'name':name,'version':'2-1','artifact':str(artifact),'sha256':tx.file_hash(artifact),
       'source_host_sha256':'fixture-host','recipe_sha256':data['recipes'][name]['recipe_sha256'],
       'target_packages_sha256':tx.file_hash(p/'target-packages.txt'),
       'target_base_sha256':tx.file_hash(p/'target-base-packages.txt'),
       'tests':{'passed':True,'artifacts':artifacts},'record':{'recipe':'fixture'}}
    tx.write_json(p/'result.json',r);results[name]=r
# Keep tx.install's exact real lock/private DBPath logic, adding only a test
# RootDir/CacheDir/HookDir so the actual package manager touches this root.
def run(command,**kw):
    if 'pacman' in command:  # last in the bind-mount wrapper: these are pacman's
        command=command+['--root',str(root),'--dbpath',str(root_db),'--cachedir',str(cache),'--hookdir',str(root/'no-hooks')]
    return subprocess.run(command,**kw)
tx.install(ref,d,data,results,lambda:'fixture-host',str(root_db),run)
# -Q only lists entry directory names; -Qi, -Qk and -Dk need complete entries.
def query(*args):
    return subprocess.run(['pacman','--root',str(root),'--dbpath',str(root_db)]+list(args),
                          check=True,capture_output=True,text=True).stdout
info=query('-Qi','fixture-compositor','fixture-tools','fixture-lib-new')
assert 'Version         : 2-1' in info and info.count('Version         : 2-1')==3,info
query('-Qk','fixture-compositor','fixture-tools','fixture-lib-new');query('-Dk')
entries=sorted(p.name for p in (root_db/'local').iterdir() if p.is_dir())
assert entries==['fixture-compositor-2-1','fixture-lib-new-2-1','fixture-lib-old-1-1','fixture-tools-2-1'],entries
assert not (root_db/'db.lck').exists()
print('PASS real fixed-snapshot pacman recorded both validated local artifacts and their new runtime dependency together')
PY
