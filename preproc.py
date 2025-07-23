class ImageProcessing:
    def __init__(self, sciezka_do_pliku):
        self.path = sciezka_do_pliku
        self.rozmiar_fragmentu = 58 # ile maksymalnie bajtów może być w kawałku
        self.fragmenty = [] # tablica na jakiś kawałek

    def wykonaj(self):
        with open(self.path, 'rb') as plik: # rb to otwieranie w trybie binarnym
            dane = plik.read()  # wczytanie całego pliku
            for i in range(0, len(dane), self.rozmiar_fragmentu):
                fragment = dane[i:i + self.rozmiar_fragmentu]
                self.fragmenty.append(fragment)
            print(f"Rozmiar całego pliku: {len(dane)} bajtów")
            """suma kontrolna - jakiś kawałaek bajtów, który po porównaniu przed i po wysłaniu pokazuje, czy jakieś dane się po drodze nie zgubiły.
            to może być np. CRC32, czyli 4 "kontrolne" bajty
            wtedy pętla :
            for i in range(0, len(dane), self.rozmiar_fragmentu):
                fragment = dane[i:i + self.rozmiar_fragmentu]
                suma_crc = zlib.crc32(fragment).to_bytes(4, 'big')  # 4 bajty sumy
                pakiet = fragment + suma_crc
                self.pakiety.append(pakiet)
            I trzeba zaimportować bibliotekę ZLIB"""
            # PYTANIE - CZY BAJTY KONTROLNE SIĘ WLICZAJĄ W PACZKĘ 58 BAJTÓW?
            print(f"Liczba fragmentów: {len(self.fragmenty)}")
