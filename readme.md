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
* [ ] Learn code obfuscation
* [ ] Package releases
* [ ] Add a licensing system
* [ ] Integrate AutoSurg into the standard license

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

---

# Updating the Repository

Using the helper script:

```bat
push.bat
```

---

# Project Structure

```text
proxy/
│
├── bin/                  # Build output (ignored by Git)
├── enet/                 # ENet source
│
├── proxy.c               # Main proxy
├── https.c               # HTTPS handling
├── hosts.c               # Hosts file modification
├── getserver.c           # Server lookup
├── packet.h
│
├── build.bat
├── push.bat
├── make_cert.bat
│
├── .gitignore
└── README.md
```


> *Drink coffee ☕.*
