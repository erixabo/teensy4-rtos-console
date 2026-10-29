# Teensy RTOS mikroszámítógép – shell és kísérleti Ethernet

Ez a különálló FreeRTOS kísérleti rendszer. A `teendds-ng` projektet és az azon
lévő Teensyt nem módosítja.

Különálló kísérleti projekt. A `teendds-ng` adóprogramhoz és az UZHB-n lévő Teensyhez nem nyúl.

## Jelenlegi alap

- FreeRTOS scheduler, USB CDC soros konzol és óra/üzemidő feladat.
- Shell: `help`, `status`, `devices`, `echo on|off`.
- A `devices` hardver szerint felsorolja a USB CDC-t, a Teensy 4.1 nyolc UART-ját, három SPI-master buszát, a microSD-t és az Ethernetet. A UART/SPI portok csak leltározva vannak; az USB CDC-n kívül jelenleg egyik sincs megnyitva vagy forgalmazva.
- FAT/exFAT SD-fájlrendszer és shellműveletek: `sd`, `ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `cat`, `put`, `rm`.
- Induláskor automatikusan csatolja az SD-kártyát; ha az hiányzik vagy hibás, nem indítja el a rendszert. Teensy 4.1-en a beépített SDIO microSD-foglalatot használja; Teensy 4.0-n SPI-s SD-adapter és CS=10 kell.
- A `put` CR/LF sorvégeket ír a fájlba; a `cat` bájtról bájtra, változtatás nélkül olvassa ki.
- Teensy 4.1-en kísérleti QNEthernet/lwIP hálózat indul DHCP-vel, `teensy-os` hostnévvel és TCP-konzollal a 2323-as porton. A TCP és USB soros kapcsolat ugyanazt a shellt és parancslistát használja. Teensy 4.0-s buildben nincs Ethernet.
- A közös shell parancsai: `help`, `status`, `devices`, `net`, `echo on|off`, `sd`, `pwd`, `cd`, `ls`, `mkdir`, `rmdir`, `cat`, `put`, `rm`; TCP-n a `quit` bontja a kapcsolatot.
- A shellműveleteket mutex sorosítja, így az SD-kártyát és az aktuális könyvtárat nem módosítja egyszerre két terminál. Hálózati feltöltés közben a TCP task tovább hívja az Ethernet feldolgozó ciklusát.
- A TCP-konzol hitelesítés nélkül engedi a fájlmódosító parancsokat. Csak saját, megbízható LAN-on használd, ne irányítsd ki az internetre. Következő fejlesztés a felhasználókezelés és a TCP-belépés védelme.
- Nincs még ramdisk, eszközfájl-réteg, assembler/monitor vagy programfuttatás.

A `put fájl` felülírja a fájlt, soronként fogad szöveget, és egy önálló `.` sor zárja le. Az `rm` fájlt töröl, az `rmdir` csak üres könyvtárat távolít el. A kártyát a program nem formázza.

## Fordítás és hálózati próba

PlatformIO Core 6.2.0 kell. A Teensy 4.1 az alapértelmezett cél; a 4.0-s változat
is fordítható. A platform, a FreeRTOS-kiegészítés és a QNEthernet a
`platformio.ini`-ben rögzített Git-revíziókhoz van kötve:

```sh
pio run -e teensy41
pio run -e teensy40
```

Ethernethez a Teensy 4.1 Ethernet-jelcsatlakozásának megfelelő magjack/PHY bekötés kell; önmagában a panelen nincs RJ45 aljzat. DHCP-cím után a soros `status` vagy `net` parancs mutatja az IP-címet. A hálózatról `nc <IP> 2323` paranccsal lehet csatlakozni.

QNEthernet verzió: `v0.37.0`, licenc: AGPL-3.0-or-later. A könyvtárat tartalmazó firmware terjesztésekor tartsd be a licenc feltételeit.

## Fordítási állapot

2026-10-29-én a `pio run -e teensy41` és `pio run -e teensy40` fordítás
ellenőrizhető. A Teensy 4.1 Ethernetes viselkedését a hálózatra csatlakoztatott
hardveren is próbáltuk; a 4.0-s cél jelenleg Ethernet nélküli fordítási cél.
Fordított firmware és PlatformIO gyorsítótár nincs a repóban.

## Korlátok

A FreeRTOS-port kísérleti, nem hivatalos Teensy-platformot és módosított Arduino core-t használ. A QNEthernet hívásai a hálózati taskban maradnak; a shell parancsvégrehajtást mutex védi. A Teensy 4.1-en 1 MiB RAM és 7.75 MiB flash van; a pontos használatot az aktuális PlatformIO build méretjelentése mutatja.

Források:

- https://github.com/tsandmann/freertos-teensy
- https://github.com/tsandmann/platform-teensy
- https://github.com/ssilverman/QNEthernet
- https://www.pjrc.com/store/teensy41.html
