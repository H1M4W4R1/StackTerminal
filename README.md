# StackTerminal

Terminal UART dla M5Stack Atom VoiceS3R (C126-ECHO) i Atomic Display Base (A115). LVGL wyświetla ciemny interfejs przez HDMI 720p. Zasilany hub USB obsługuje klawiaturę HID Boot w układzie polskim programisty.

## Uruchomienie

1. Podłącz Atom bezpośrednio do komputera przez USB-C i wprowadź go w tryb pobierania zgodnie z [instrukcją M5Stack](https://docs.m5stack.com/en/core/Atom_VoiceS3R#learn).
2. Uruchom `pio run -t upload` z katalogu projektu.
3. Podłącz A115 z monitorem HDMI oraz klawiaturę przez hub. Wpisz znak i sprawdź, czy pojawi się w polu u dołu ekranu.
4. Podłącz UART zgodnie z tabelą i w `Ctrl+Tab` ustaw parametry urządzenia. Pierwszy start używa 115200 8N1.

## Grove i USB

| Przewód Grove | Pin Atoma | Połączenie z urządzeniem |
| --- | --- | --- |
| Czarny | GND | Wspólna masa |
| Biały | GPIO1 / RX | TX urządzenia |
| Żółty | GPIO2 / TX | RX urządzenia |
| Czerwony | 5 V | Pozostaw odłączony przy osobnym zasilaniu urządzenia |

UART używa logiki 3,3 V. Dla sygnałów 5 V, RS-232 lub RS-485 dobierz odpowiedni konwerter. Czerwony przewód Grove jest linią zasilania. [Pinout Atom VoiceS3R](https://docs.m5stack.com/en/core/Atom_VoiceS3R#pinmap)

Po stronie Atoma potrzebne są dane USB 2.0 i zasilanie 5 V. USB-C Atoma ma rezystory wejścia zasilania na CC. Typowy zasilany hub dostarcza prąd swoim urządzeniom, więc sprawdź, czy wybrany hub i adapter przekazują też 5 V do Atoma przy zachowaniu danych. Jeśli potrzebujesz osobnego zasilania Atoma, podaj regulowane 5 V przez Grove i użyj wspólnej masy z UART. Przy takim zasilaniu odseparuj VBUS adaptera, aby uniknąć łączenia dwóch źródeł 5 V. [Schemat Atoma](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1182/Sch_M5_AtomS3R_v0.4.1.pdf), [ESP-IDF USB Host](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/peripherals/usb_host.html)

## Klawiatura

| Klawisz | Działanie |
| --- | --- |
| Enter | Wyślij treść i dodaj na końcu bajty CR oraz LF |
| Shift+Enter | Wstaw CR oraz LF w miejscu kursora, bez wysyłania |
| Ctrl+Enter | Wyślij treść bez dopisywania końcówki |
| ↑ / ↓ | Przywołaj wysłane polecenia. Zejście za najnowsze przywraca szkic |
| ← / →, Home / End | Przesuń kursor lub skocz na początek / koniec wiersza |
| Insert | Przełącz wstawianie i nadpisywanie |
| PageUp / PageDown | Przewiń odebrane wiersze o stronę. Ostatnia strona śledzi nowe dane |
| Ctrl+↑ / Ctrl+↓ | Zmień rozmiar czcionki |
| Ctrl+Tab | Otwórz ustawienia UART i kolorów |
| Prawy Alt + litera | Wpisz polski znak |
| Ctrl+A do Ctrl+Z | Wstaw kod sterujący ASCII od SOH do SUB |

W ustawieniach wybierz pole przez ↑/↓. Zmień wartość przez ←/→ lub wpisz prędkość i sześciocyfrowe kolory RGB. Enter zapisuje ustawienia i aktualny rozmiar czcionki w NVS. Esc lub Ctrl+Tab zamyka panel bez zapisu.

Odbiór dzieli tekst przy CR, LF i parze CR+LF. Para CR+LF zajmuje jeden wiersz historii i jest pokazana jako dwie etykiety. Pozostałe znaki sterujące też mają obramowaną nazwę, np. SOH, TAB i ESC. Terminal obsługuje tekst UTF-8. Przechowuje 512 odebranych wierszy i 100 wysłanych poleceń w RAM. Długi wiersz zawija się na ekranie, a odbiór pojedynczego wiersza jest ograniczony do 2048 znaków.

## Szybki test sprzętu

1. Podaj przez UART bajty `41 01 0D 0A 42 0A 43 0D`. Ekran powinien pokazać trzy wiersze. W pierwszym po `A` powinny być widoczne osobne etykiety `SOH`, `CR` i `LF`.
2. Wpisz `A`, naciśnij Shift+Enter, wpisz `B`, po czym naciśnij Ctrl+Enter. Urządzenie powinno dostać `41 0D 0A 42`.
3. Zmień kolor i prędkość przez Ctrl+Tab, zapisz Enterem i uruchom Atom ponownie. Sprawdź, czy wrócił z zapisanymi ustawieniami.

## Diagnostyka huba i klawiatury USB

1. Wgraj wariant diagnostyczny poleceniem `pio run -e stackterminal-usb-diag -t upload`.
2. Uruchom Atom z zasilanym hubem i monitorem HDMI. Odłącz klawiaturę od huba, podłącz ją z powrotem i naciśnij jeden klawisz.
3. Odczytaj komunikaty USB z historii na HDMI. `USB root` pokazuje, czy kontroler Atoma wykrył fizyczne podłączenie huba. `logical power` to stan kontrolera, bez pomiaru 5 V na VBUS. Kolejne komunikaty pokazują liczbę urządzeń, wykrycie klawiatury, jej odłączenie i długość pierwszego raportu.

Polecenie `pio run -t upload` wgrywa zwykły wariant.

Czcionki pochodzą z [JetBrains Mono 2.304](https://github.com/JetBrains/JetBrainsMono/tree/v2.304). Wersje bitmapowe przygotowano przy użyciu `lv_font_conv 1.5.3`. Licencja znajduje się w [src/fonts/OFL.txt](src/fonts/OFL.txt).
