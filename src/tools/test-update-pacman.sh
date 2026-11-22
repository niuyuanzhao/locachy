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
tx.install(ref,d,data,results,lambda:'fixture-host',str(root_db),run,record=str(out/'sync-published.json'))
import json
assert json.loads((out/'sync-published.json').read_text())=={'extra.db':tx.file_hash(root_db/'sync/extra.db')}
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

# Second real-pacman case: same-version AUR package (python-dlib 20.1-1) linked
# to libjxl.so=0.12-64 blocks libjxl 0.13.0-1.1 until rebuilt in the prepared
# target root and installed in ONE transaction, even when the host cache still
# holds the stale same-version archive and python-dlib is marked --asdeps.
root2=out/'root2';cache2=out/'cache2';rebuilt_dir=out/'rebuilt'
(root2/'var/lib/pacman/local').mkdir(parents=True);(root2/'var/lib/pacman/sync').mkdir();cache2.mkdir();rebuilt_dir.mkdir()
(root2/'tmp').mkdir();(root2/'no-hooks').mkdir()
db2=root2/'var/lib/pacman'
conf2=out/'aur.conf'
conf2.write_text('[options]\nArchitecture = x86_64\nSigLevel = Never\nLocalFileSigLevel = Never\n[extra]\nServer = '+cache2.as_uri()+'\n')
def archive2(name,version,depend=(),provides=(),dst=cache2,body=None,bdate=1):
    path=dst/(name+'-'+version+'-any.pkg.tar.gz')
    text=f'pkgname = {name}\npkgver = {version}\npkgdesc = fixture\nurl = https://example.invalid\nbuilddate = {bdate}\npackager = test\nsize = 1\narch = any\nlicense = MIT\n'
    for dep in depend:text+='depend = '+dep+'\n'
    for prov in provides:text+='provides = '+prov+'\n'
    content=(body or (name+' '+version+'\n')).encode()
    mtree=(f'#mtree\n/set type=file uid=0 gid=0 mode=644\n'
           f'./.PKGINFO size={len(text.encode())}\n'
           f'./usr type=dir mode=755\n'
           f'./usr/share type=dir mode=755\n'
           f'./usr/share/locachy-fixture type=dir mode=755\n'
           f'./usr/share/locachy-fixture/{name} size={len(content)}\n').encode()
    with tarfile.open(path,'w:gz') as t:
        data=text.encode();i=tarfile.TarInfo('.PKGINFO');i.size=len(data);t.addfile(i,io.BytesIO(data))
        data=gzip.compress(mtree);i=tarfile.TarInfo('.MTREE');i.size=len(data);t.addfile(i,io.BytesIO(data))
        i=tarfile.TarInfo('usr/share/locachy-fixture/'+name);i.size=len(content);i.mode=0o644;t.addfile(i,io.BytesIO(content))
    return path
jxl12=archive2('libjxl','0.12.0-1',provides=('libjxl.so=0.12-64',))
dlib12=archive2('python-dlib','20.1-1',depend=('libjxl.so=0.12-64',))
howdy=archive2('howdy-git','3.0-1',depend=('python-dlib',))
jxl13=archive2('libjxl','0.13.0-1.1',provides=('libjxl.so=0.13-64',))
dlib13=archive2('python-dlib','20.1-1',depend=('libjxl.so=0.13-64',),dst=rebuilt_dir,body='rebuilt 0.13\n',bdate=2)
def pacman2(args):
    return subprocess.run(['pacman','--root',str(root2),'--dbpath',str(db2),'--config',str(conf2),
        '--cachedir',str(cache2),'--hookdir',str(root2/'no-hooks'),'--noconfirm']+args,check=True)
pacman2(['-U',str(jxl12),str(dlib12),str(howdy)])
pacman2(['-D','--asdeps','python-dlib'])
subprocess.run(['repo-add',str(cache2/'extra.db.tar.gz'),str(jxl13)],check=True,stdout=subprocess.DEVNULL)
shutil.copyfile(cache2/'extra.db.tar.gz',db2/'sync/extra.db')
import importlib.machinery
loader=importlib.machinery.SourceFileLoader('pkg_test',str(src/'link/locachy-pkg'))
spec=importlib.util.spec_from_loader('pkg_test',loader);pkg_mod=importlib.util.module_from_spec(spec);loader.exec_module(pkg_mod)
pkg_mod.MERGED_DB=str(db2);pkg_mod.PACMAN_CONFIG=str(conf2)
blockers=pkg_mod.blocking_aur_packages({'python-dlib':'20.1-1','howdy-git':'3.0-1'},
    {'python-dlib':'20.1-1','howdy-git':'3.0-1'},set())
assert blockers=={'python-dlib':'rebuild for libjxl'},blockers
ref2=tx.snapshot(out/'state2',db2/'sync',conf2,'fixture-host2',{},
    aur={'python-dlib':{'installed_version':'20.1-1','target_version':'20.1-1','reason':blockers['python-dlib'],'provides':[]}})
d2,data2=tx.load_snapshot(ref2,'fixture-host2')
p_aur=d2/'prepared/python-dlib';p_aur.mkdir(parents=True)
art_aur=p_aur/dlib13.name;shutil.copyfile(dlib13,art_aur)
(p_aur/'target-base-packages.txt').write_text('howdy-git 3.0-1\nlibjxl 0.13.0-1.1\n')
(p_aur/'target-packages.txt').write_text('howdy-git 3.0-1\nlibjxl 0.13.0-1.1\npython-dlib 20.1-1\n')
h_aur=tx.file_hash(art_aur)
res2={'python-dlib':{'name':'python-dlib','kind':'aur','version':'20.1-1','artifact':str(art_aur),'sha256':h_aur,
    'source_host_sha256':'fixture-host2','patch_sha256':None,'extra_artifacts':[],
    'target_base_sha256':tx.file_hash(p_aur/'target-base-packages.txt'),
    'target_packages_sha256':tx.file_hash(p_aur/'target-packages.txt'),
    'tests':{'passed':True,'id':tx.AUR_SMOKE_GATE,'artifact_sha256':h_aur,'artifacts':{'python-dlib':h_aur},'test_hash':'0'*64}}}
tx.write_json(p_aur/'result.json',res2['python-dlib'])
def run2(command,**kw):
    if 'pacman' in command:
        command=command+['--root',str(root2),'--dbpath',str(db2),'--cachedir',str(cache2),'--hookdir',str(root2/'no-hooks')]
    return subprocess.run(command,**kw)
tx.install(ref2,d2,data2,res2,lambda:'fixture-host2',str(db2),run2,record=str(out/'sync-published2.json'))
qi=subprocess.run(['pacman','--root',str(root2),'--dbpath',str(db2),'-Qi','python-dlib','libjxl'],
    check=True,capture_output=True,text=True).stdout
assert 'Depends On      : libjxl.so=0.13-64' in qi,qi
assert 'Install Reason  : Installed as a dependency for another package' in qi,qi
assert 'Version         : 0.13.0-1.1' in qi,qi
assert (root2/'usr/share/locachy-fixture/python-dlib').read_text()=='rebuilt 0.13\n'
subprocess.run(['pacman','--root',str(root2),'--dbpath',str(db2),'-Dk'],check=True,stdout=subprocess.DEVNULL)
assert not (db2/'db.lck').exists()
print('PASS real pacman rebuilt same-version AUR blocker and upgraded libjxl in one transaction')
PY
