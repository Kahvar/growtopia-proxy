```
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀ /\_/\\
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀  ( o.o )
⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀ > ^ <
══════════════════════════════════════════════════════
               G R O W T O P I A   P R O X Y
══════════════════════════════════════════════════════
```

> **Status:** 🚧 Active Development
>
> A personal Growtopia man-in-the-middle proxy for reverse engineering, packet inspection, and packet modification.

---

# TODO

* [ ] Get login working like GTProxy
* [ ] Read packets correctly
* [ ] Modify packets
* [ ] Implement basic features

  * [ ] Pathfind
  * [ ] Autohost
  * [ ] `/dd`
  * [ ] `/cd`
  * [ ] `/spam`

---

# Requirements

Install the following before building.

## Git

Used for version control.

https://git-scm.com/download/win

---

## MinGW-w64 (GCC)

Required to compile the project.

Official website:

https://www.mingw-w64.org/

Recommended Windows build:

https://winlibs.com/

Verify the installation:

```bash
gcc --version
```

---

## OpenSSL

Required for TLS/HTTPS support.

Downloads:

https://slproweb.com/products/Win32OpenSSL.html

Official documentation:

https://openssl.org/

---

# Building

Open a Command Prompt inside the project directory and run:

```bat
build.bat
```

If the build succeeds, the executable will be placed in:

```text
bin/
```

---

# Git clone

Clone the repository:

```bash
git clone https://github.com/Kahvar/growtopia-proxy.git
cd growtopia-proxy
```

Install the requirements above, then simply run:

```bat
build.bat
```

No source files need to be copied manually.



> *Drink coffee ☕.*
