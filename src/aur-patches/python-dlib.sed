# The AUR's python-dlib (howdy's dlib) as the CPU build alone: its recipe
# builds python-dlib and python-dlib-cuda, the second with cuda and cudnn,
# unless _build_cuda is 0. locachy-build applies this to the AUR's PKGBUILD
# of the moment when python-dlib is asked for or needed (settings: faces
# computed on the CPU).
s/^_build_cuda=1$/_build_cuda=0/
