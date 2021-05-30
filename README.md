

## Build

```shell
$ meson --buildtype=release build
$ ninja -C build

# optional for installing the plugin
$ sudo ninja -C build install
```

It will install into the obs-plugins directory inside whatever libdir in meson is set to.

E.g.
```shell
meson --buildtype=release --libdir=lib build
```
will install at /usr/local/lib/obs-plugins.


If you want it to install outside of /usr/local you will have to set a prefix as well.

E.g.
```shell
meson --buildtype=release --libdir=lib --prefix=/usr build
```
You can also make it install in your user home directory (wherever that directory was exactly..)

## Run

If you don't want to install the plugin, instruct OBS to look it up in the build directory:

```shell
$ export OBS_PLUGINS_PATH=$PWD/build/
$ obs
```
