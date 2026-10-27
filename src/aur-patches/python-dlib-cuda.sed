# The AUR's python-dlib as its CUDA build alone (python-dlib-cuda, which
# provides python-dlib): the recipe's CPU build left out with _build_cpu=0.
# For a machine with an NVIDIA GPU and its driver (settings: faces computed
# on the NVIDIA GPU). The recipe counts on a login shell's environment, which
# finds cuda through /etc/profile.d/cuda.sh (/opt/cuda/bin, CUDA_PATH,
# NVCC_CCBIN); a build installs cuda only as it starts, so its build reads
# /etc/profile first. dlib turns CUDA off when it cannot use it ("DLIB WILL
# NOT USE CUDA"): then the build fails rather than package a dlib without it.
s/^_build_cpu=1$/_build_cpu=0/
/^build_python-dlib-cuda() {$/a\
  source /etc/profile
/DLIB_USE_CUDA=ON python setup.py build/a\
  grep -Eq '^DLIB_USE_CUDA:[A-Z]+=(ON|1|TRUE)$' build/temp*/CMakeCache.txt || { echo 'dlib did not find CUDA' >&2; return 1; }
