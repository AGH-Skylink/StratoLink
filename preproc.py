import zlib
class ImageProcessing:
    def __init__(self, sciezka_do_pliku):
        self.path = sciezka_do_pliku
        self.rozmiar_fragmentu = 54 # ile maksymalnie bajtów może być w kawałku -> mamy sumę kontrolną, 4 bajty w tych 58, więc bez będzie 54
        self.fragmenty = [] # tablica na jakiś kawałek

    def wykonaj(self):
        with open(self.path, 'rb') as plik: # rb to otwieranie w trybie binarnym
            dane = plik.read()  # wczytanie całego pliku
            for i in range(0, len(dane), self.rozmiar_fragmentu):
                fragment = dane[i:i + self.rozmiar_fragmentu]
                suma_crc = zlib.crc32(fragment).to_bytes(4, 'big')  # 4 bajty sumy
                pakiet = fragment + suma_crc
                self.pakiety.append(pakiet)
            print(f"Liczba fragmentów: {len(self.fragmenty)}")
