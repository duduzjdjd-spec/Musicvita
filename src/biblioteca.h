// =====================================================================
//                    TU MUSICA  (edita solo este archivo)
// =====================================================================
// Pega cada link entre comillas y termina la linea con una coma.
// Las lineas vacias "" se ignoran, asi que puedes dejar espacios libres.
// Despues de editar: "Commit changes", espera el check verde en Actions e instala el nuevo VPK.

// ---------------------------------------------------------------------
// YOUTUBE (menu "Buscar en YouTube (Topic)"): no necesita PC.
// Usa instancias publicas de Piped (un servicio abierto que busca en YouTube y da el audio).
// La app prueba una por una hasta que una responde, y si todas fallan baja sola la lista oficial
// de https://github.com/TeamPiped/documentation (public-instances).
// Si una instancia deja de funcionar, agrega otra aqui (o una por linea en
// ux0:data/MusicVita/piped.txt en la Vita, sin recompilar). Formato: "https://pipedapi.ejemplo.com"
// ---------------------------------------------------------------------
static const char *PIPED[] = {
  "https://pipedapi.kavin.rocks",
  "https://pipedapi.leptons.xyz",
  "https://pipedapi.nosebs.ru",
  "https://pipedapi-libre.kavin.rocks",
  "https://piped-api.privacy.com.de",
  "https://pipedapi.adminforge.de",
  "https://api.piped.yt",
  "https://pipedapi.drgns.space",
  "https://pipedapi.owo.si",
  "https://pipedapi.ducks.party",
  "https://piped-api.codespace.cz",
  "https://pipedapi.reallyaweso.me",
  "https://api.piped.private.coffee",
  "https://pipedapi.darkness.services",
  "https://pipedapi.orangenet.cc",
  "",
};

// ---------------------------------------------------------------------
// ALBUMES de Internet Archive: el link de la pagina del album.
//   Ejemplo: "https://archive.org/details/gd1977-05-08.shure57.stevenson.29303.flac16",
// La app saca sola el titulo, el artista, la portada y la lista de canciones.
// ---------------------------------------------------------------------
static const char *ALBUMES[] = {
  "https://archive.org/details/rustilldownoriginalalbum1997",
  "https://archive.org/details/09-one-and-only_202505",
  "https://archive.org/details/theend-bep",
  "https://archive.org/details/The-Black-Eyed-Peas-The-Beginning",
  "",
  "",
  "",
  "",
  "",
  "",
};

// ---------------------------------------------------------------------
// CANCIONES sueltas: link directo al archivo MP3.
//   De Internet Archive: "https://archive.org/download/IDENTIFICADOR/archivo.mp3"
//   (tambien sirve "https://archive.org/details/IDENTIFICADOR/archivo.mp3")
//   Puedes ponerle titulo y artista con este formato:  "Titulo | Artista | link"
//   Ejemplo: "Mi cancion | Mi artista | https://archive.org/download/algo/cancion.mp3",
// Si solo pegas el link, el titulo sera el nombre del archivo.
// ---------------------------------------------------------------------
static const char *CANCIONES[] = {
  "",
  "",
  "",
  "",
  "",
  "",
  "",
  "",
  "",
  "",
};
