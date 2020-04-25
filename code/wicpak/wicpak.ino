#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <ESP8266mDNS.h>

// Include title.h which includes a list of
// N64 game titles.
#include "title.h"

// Some defines.
#define CPAKBYTES 32768
#define CPAKHALF (CPAKBYTES / 2)
#define MAXADDR (CPAKBYTES - 1)
#define NCE_MEM_PIN 16
#define NOE_MEM_PIN 0
#define NWE_MEM_PIN 5
#define SHIFT_CLK_PIN 15
#define ADDRSHIFT_PIN 14
#define DATSHIFT_NOE_PIN 2
#define DATSHIFT_MODE_PIN 12
#define DATSHIFT_DIN_PIN 13
#define DATSHIFT_DOUT_PIN 4

// SSID
const char* ssid = "WiCPak64";

// CPak filename
const char* cpakfile = "/wicpak.mpk";

// Server
ESP8266WebServer server( 80 );

// We need to store the uploaded file.
File fsUpload;

// CPak content array.
char cpakArr[ CPAKHALF ];

// Flag set high if the CPak should be flashed.
char updateCPak = 0;


// CPak parsing function declarations.
char getChar( uint8_t c );
uint16_t getHeaderChkSum( uint8_t* arr, unsigned int offset );
bool validHeader( uint8_t* arr );
unsigned int readEntries( uint8_t* arr, char entries[][17] );
const char* getGameTitle( char c0, char c1, char c2 );


// Main page HTML.
const char* mainHeader = 
"<h1>WiCPak 64</h1>\
<p><a href=\"getpak\">Click here to download CPak.</a></p>";

const char* mainUp =
"<p>Upload below.</p>\
<form method=\"post\" enctype=\"multipart/form-data\">\
<input type=\"file\" name=\"name\">\
<input class=\"button\" type=\"submit\" value=\"Upload to CPak\">\
</form>";

// Struct holding entry informations.
struct gameEntry {
  char entryName[ 17 ];
  const char* gameID;
  char region;
};

// Struct holding CPak infos.
struct cpakInfo {
  struct gameEntry entries[ 16 ];
  int nrEntries;
} curCPakInfo;


// Server functions.
void handleRoot() {
  Serial.println( "Handling root GET" );
  // Create main page.
  String mainPage( mainHeader );
  // Create the info part.
  mainPage += "<p><b>CPak Status</b></p>";
  if ( curCPakInfo.nrEntries == -1 ) {
    mainPage += "<p><i>Corrupt</i></p>";
  } else if ( curCPakInfo.nrEntries == 0 ) {
    mainPage += "<p><i>Empty</i></p>";
  } else {
    for ( int i = 0; i < curCPakInfo.nrEntries; ++i ) {
      mainPage += "<p>";
      mainPage += curCPakInfo.entries[ i ].entryName;
      mainPage += "<i> (";
      mainPage += curCPakInfo.entries[ i ].gameID;
      mainPage += " [";
      mainPage += curCPakInfo.entries[ i ].region;
      mainPage += "])</i>";
      mainPage += "</p>";
    }
  }

  mainPage += "<br>";
  mainPage += mainUp;

  // Send the main page.
  server.send( 200, "text/html", mainPage );
}

void handleDownload() {
  // Stream the current mem pak.
  // Open the mem pak file.
  File f = SPIFFS.open( cpakfile, "r");
  // Send it.
  size_t sentBytes = server.streamFile( f, "application/x-binary" );
  f.close();
  Serial.print( "Sent bytes:" );
  Serial.println( sentBytes );
}

void handle404() {
  server.send( 404, "text/plain", "Not found." );
}

void sendOK() {
  server.send( 200 );
}

void handleFileUpload() {
  // TBD: Assert size.
  
  // Receive the file and store it into the file system.
  HTTPUpload& upload = server.upload();
  if ( upload.status == UPLOAD_FILE_START ) {
    // Open the cpak file to write.
    fsUpload = SPIFFS.open( cpakfile, "w" );
  } else if ( upload.status == UPLOAD_FILE_WRITE ) {
    if ( fsUpload ) {
      fsUpload.write( upload.buf, upload.currentSize );
    }
  } else if ( UPLOAD_FILE_END ) {
    if ( fsUpload ) {
      fsUpload.close();
      Serial.println( "Received file" );
      // Send a response.
      server.sendHeader( "Location", "/" );
      server.send( 303 );
      updateCPak = 1;
    } else {
      server.send( 500, "text/plain", "500: Error creating file." );
    }
  }
}

// Memory read and write functions.
void shiftAddrBit( char b ) {
  // The 74HC595D takes over at a rising edge.
  digitalWrite( SHIFT_CLK_PIN, 0 );
  digitalWrite( ADDRSHIFT_PIN, b );
  // Wait a bit.
  delayMicroseconds( 1 );
  // Rise edge.
  digitalWrite( SHIFT_CLK_PIN, 1 );
  // Hold.
  delayMicroseconds( 1 );
  digitalWrite( SHIFT_CLK_PIN, 0 );
}

void advanceAddrShift() {
  digitalWrite( SHIFT_CLK_PIN, 0 );
  // Wait a bit.
  delayMicroseconds( 1 );
  // Rise edge.
  digitalWrite( SHIFT_CLK_PIN, 1 );
  // Hold.
  delayMicroseconds( 1 );
  digitalWrite( SHIFT_CLK_PIN, 0 );
}

void datRegLoadMode() {
  // Set mode.
  digitalWrite( DATSHIFT_MODE_PIN, 1 );
  // Wait a bit.
  delayMicroseconds( 1 );
}

void datRegShiftMode() {
  digitalWrite( DATSHIFT_MODE_PIN, 0 );
  // Wait a bit.
  delayMicroseconds( 1 );
}

void datRegShiftClk() {
  digitalWrite( SHIFT_CLK_PIN, 0 );
  // Wait a bit.
  delayMicroseconds( 1 );
  digitalWrite( SHIFT_CLK_PIN, 1 );
  // Hold.
  delayMicroseconds( 1 );
  digitalWrite( SHIFT_CLK_PIN, 0 );
}

void shiftDataBit( char b ) {
  // We assume that the data reg is alread set into 
  // serial shift mode.
  digitalWrite( SHIFT_CLK_PIN, 0 );
  digitalWrite( DATSHIFT_DOUT_PIN, b );
  // Wait a bit.
  delayMicroseconds( 1 );
  // Rise edge.
  digitalWrite( SHIFT_CLK_PIN, 1 );
  // Hold.
  delayMicroseconds( 1 );
  digitalWrite( SHIFT_CLK_PIN, 0 );
}

// Shift in a bit in the address *and* data register.
void shiftBitBoth( char addr, char data ) {
  // We assume that the data reg is alread set into 
  // serial shift mode.
  // Both regs take over at a rising edge.
  digitalWrite( SHIFT_CLK_PIN, 0 );
  digitalWrite( ADDRSHIFT_PIN, addr );
  digitalWrite( DATSHIFT_DOUT_PIN, data );
  // Wait a bit.
  delayMicroseconds( 1 );
  // Rise edge.
  digitalWrite( SHIFT_CLK_PIN, 1 );
  // Hold.
  delayMicroseconds( 1 );
  digitalWrite( SHIFT_CLK_PIN, 0 );
}

void FRAMRead() {
  // Make sure we are not writing.
  digitalWrite( NWE_MEM_PIN, 1 );
  // Pull /CE down.
  digitalWrite( NCE_MEM_PIN, 0 );
  // Pull /OE down.
  digitalWrite( NOE_MEM_PIN, 0 );
}

void FRAMWrite() {
  // Make sure the FRAM /OE is high.
  digitalWrite( NOE_MEM_PIN, 1 );
  // Enable output of the data reg.
  digitalWrite( DATSHIFT_NOE_PIN, 0 );

  // Do the actual write.
  digitalWrite( NCE_MEM_PIN, 0 );
  digitalWrite( NWE_MEM_PIN, 0 );

  // Wait a bit.
  delayMicroseconds( 1 );

  // And finish.
  FRAMIdle();

  // Disable data reg output.
  digitalWrite( DATSHIFT_NOE_PIN, 1 );
}

void FRAMIdle() {
  // Make sure we are not writing.
  digitalWrite( NWE_MEM_PIN, 1 );
  // Pull /CE up.
  digitalWrite( NCE_MEM_PIN, 1 );
  // Pull /OE up.
  digitalWrite( NOE_MEM_PIN, 1 );
}

void datRegDisableOut() {
  digitalWrite( DATSHIFT_NOE_PIN, 1 );
}

void datRegEnableOut() {
  digitalWrite( DATSHIFT_NOE_PIN, 0 );
}

void readMemPakRange( int start, int el ) {
  // Always start at zero offset.
  int ind = 0;
  for ( unsigned int addr = start; ind < el; ++addr, ++ind ) {
    // Shift in the address bits.
    for ( unsigned int i = 0; i < 16; ++i ) {
      char curBit = ( addr & ( 1 << ( 15 - i ) ) );
      // Into the shift register.
      shiftAddrBit( curBit );
    }
    
    // One last time that the output is overtaken
    // into the output FFs.
    advanceAddrShift();

    // Perform a read memory.
    FRAMRead();

    // Set the data reg into load mode.
    datRegLoadMode();

    // Load it.
    datRegShiftClk();

    // Finish the FRAM read.
    FRAMIdle();

    // And shift it it.
    datRegShiftMode();

    char curByte = 0;
    for ( unsigned int i = 0; i < 8; ++i ) {
      char curBit = digitalRead( DATSHIFT_DIN_PIN );
      curByte = curByte | ( curBit << ( 7 - i ) );
      // Advance.
      datRegShiftClk();
    }

    // Store it.
    cpakArr[ ind ] = curByte;

    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }
}

void readMemPak() {
  // Make sure the shiftreg clk is low.
  digitalWrite( SHIFT_CLK_PIN, 0 );

  // Make sure that the data shift reg's output
  // is disabled.
  datRegDisableOut();
  // Read the first half.
  readMemPakRange( 0, CPAKHALF );

  // Update the game info.
  readEntries( (uint8_t*) cpakArr );

  // Store into the flash.
  File f = SPIFFS.open( cpakfile, "w" );
  for ( unsigned int addr = 0; addr < CPAKHALF; ++addr ) {
    f.write( cpakArr[ addr ] );
  }

  // And the second half.
  readMemPakRange( CPAKHALF, CPAKHALF );

  // And write the rest into the Flash.
  for ( unsigned int addr = 0; addr < CPAKHALF; ++addr ) {
    f.write( cpakArr[ addr ] );
  }
  
  f.close();
}

void writeMemPakRange( int start, int el ) {
  int ind = 0;
  for ( unsigned int addr = start; ind < el; ++addr, ++ind ) {
    // Shift in the first 8 address bits.
    for ( unsigned int i = 0; i < 8; ++i ) {
      char curBit = ( addr & ( 1 << ( 15 - i ) ) );
      // Into the shift register.
      shiftAddrBit( curBit );
    }

    // Shift in the remaining 7 address bits and 7 of the 
    // data bits.
    for ( unsigned int i = 0; i < 7; ++i ) {
      char curAddrBit = ( addr & ( 1 << ( 15 - ( i + 8 ) ) ) );
      char curDatBit = ( cpakArr[ ind ] & ( 1 << ( 8 - i ) ) );
      // Into the shift register.
      shiftBitBoth( curAddrBit, curDatBit );
    }

    // And the last data bit. This also does the last 
    // clk shift into the output latches of the address reg.
    shiftDataBit( cpakArr[ ind ] & 0b1 );

    // And perform the write.
    FRAMWrite();

    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }
}

void writeMemPak() {
  // Start by copying the CPAK content from the file
  // to the array (first half).
  File f = SPIFFS.open( cpakfile, "r" );
  // Read in byte by byte.
  if ( f.size() < CPAKBYTES ) {
    Serial.println( "CPak file is shorter than 4096!" );
  }
  
  for ( int i = 0; i < CPAKHALF; ++i ) {
    if ( i >= f.size() )
      break;

    cpakArr[ i ] = f.read();
    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }

  // Update the game info.
  readEntries( (uint8_t*) cpakArr );

  // Write first half.
  writeMemPakRange( 0, CPAKHALF );

  // And the second half.
  for ( int i = 0; i < CPAKHALF; ++i ) {
    if ( i + CPAKHALF >= f.size() )
      break;

    cpakArr[ i ] = f.read();
    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }
  f.close();

  // Write the second half.
  writeMemPakRange( CPAKHALF, CPAKHALF );
  Serial.println( "Wrote to mempack" );
}

void setup() {
  // At very first, set some default pin values.
  // Disable data register output.
  pinMode( DATSHIFT_NOE_PIN, OUTPUT );
  digitalWrite( DATSHIFT_NOE_PIN, 1 );

  // Disable the memory output and so on.
  pinMode( NOE_MEM_PIN, OUTPUT );
  digitalWrite( NOE_MEM_PIN, 1 );
  pinMode( NCE_MEM_PIN, OUTPUT );
  digitalWrite( NCE_MEM_PIN, 1 );
  pinMode( NWE_MEM_PIN, OUTPUT );
  digitalWrite( NWE_MEM_PIN, 1 );

  // And the rest.
  pinMode( SHIFT_CLK_PIN, OUTPUT );
  digitalWrite( SHIFT_CLK_PIN, 0 );
  pinMode( ADDRSHIFT_PIN, OUTPUT );
  digitalWrite( ADDRSHIFT_PIN, 0 );
  pinMode( DATSHIFT_MODE_PIN, OUTPUT );
  digitalWrite( DATSHIFT_MODE_PIN, 0 );
  pinMode( DATSHIFT_DOUT_PIN, OUTPUT );
  digitalWrite( DATSHIFT_DOUT_PIN, 0 );
  pinMode( DATSHIFT_DIN_PIN, INPUT );
  
  // We do not need th SW watchdog
  ESP.wdtDisable();
  
  Serial.begin( 115200 );
  Serial.println( "Started" );
  WiFi.softAP( ssid );

  Serial.print( "Server IP: " );
  Serial.println( WiFi.softAPIP() );

  // Set up DNS.
  if ( !MDNS.begin( "wicpak64" ) ) {
    Serial.println( "Error setting up DNS" );
  }

  Serial.println( "DNS set up" );

  // Start filesystem.
  SPIFFS.begin();

  // Handle root access GET.
  server.on( "/", HTTP_GET, handleRoot );

  // Handle the upload.
  server.on( "/", HTTP_POST, sendOK, handleFileUpload );

  // Handle the memory pak download.
  server.on( "/getpak", HTTP_GET, handleDownload );

  // Handle everything else.
  server.onNotFound( handle404 );

  server.begin();

  // We start by reading in the current content of the
  // memory pak.
  readMemPak();

  // And read in the infos.
  readEntries( (uint8_t*) cpakArr );
}

void loop() {
  // Check for clients.
  server.handleClient();

  // Check if a new CPak file was uploaded.
  if ( updateCPak ) {
    writeMemPak();
    updateCPak = 0;
  }
}

// CPak parsing functions.

// Get the corresponding char.
char getChar( uint8_t c ) {
  switch( c ) {
    case 0:
      return 0;
    case 15:
      return ' ';
    case 16:
      return '0';
    case 17:
      return '1';
    case 18:
      return '2';
    case 19:
      return '3';
    case 20:
      return '4';
    case 21:
      return '5';
    case 22:
      return '6';
    case 23:
      return '7';
    case 24:
      return '8';
    case 25:
      return '9';
    case 26:
      return 'A';
    case 27:
      return 'B';
    case 28:
      return 'C';
    case 29:
      return 'D';
    case 30:
      return 'E';
    case 31:
      return 'F';
    case 32:
      return 'G';
    case 33:
      return 'H';
    case 34:
      return 'I';
    case 35:
      return 'J';
    case 36:
      return 'K';
    case 37:
      return 'L';
    case 38:
      return 'M';
    case 39:
      return 'N';
    case 40:
      return 'O';
    case 41:
      return 'P';
    case 42:
      return 'Q';
    case 43:
      return 'R';
    case 44:
      return 'S';
    case 45:
      return 'T';
    case 46:
      return 'U';
    case 47:
      return 'V';
    case 48:
      return 'W';
    case 49:
      return 'X';
    case 50:
      return 'Y';
    case 51:
      return 'Z';
    case 52:
      return '!';
    case 53:
      return '"';
    case 54:
      return '#';
    case 55:
      return '\'';
    case 56:
      return '*';
    case 57:
      return '+';
    case 58:
      return ',';
    case 59:
      return '-';
    case 60:
      return '.';
    case 61:
      return '/';
    case 62:
      return ':';
    case 63:
      return '=';
    case 64:
      return '?';
    case 65:
      return '@';
    default:
      return '?';
  }
}

// Get the header check sum.
uint16_t getHeaderChkSum( uint8_t* arr, unsigned int offset ) {
  uint16_t chk = 0;
  for ( unsigned int i = 0; i < 28; i += 2 ) {
    chk += ( arr[ offset + i ] << 8 ) + arr[ offset + i + 1 ];
  }
  
  return chk;
}

// Check if a CPak has a valid header.
bool validHeader( uint8_t* arr ) {
  // Check the header checksums.
  unsigned int offsets[] = { 0x20, 0x60, 0x80, 0xC0 };
  
  bool valid = true;
  for ( int i = 0; i < 4; ++i ) {
    uint16_t chk = getHeaderChkSum( arr, offsets[ i ] );
    uint16_t cmpSum = ( arr[ offsets[ i ] + 0x1C ] << 8 ) + 
                      ( arr[ offsets[ i ]+ 0x1D ] );
    if ( chk != cmpSum ) {
      valid = false;
      break;
    }
    
    cmpSum = 0xFFF2 - ( ( arr[ offsets[ i ] + 0x1E ] << 8 ) + 
                      ( arr[ offsets[ i ] + 0x1F ] ) );
                      
    if ( chk != cmpSum ) {
      valid = false;
      break;
    }
    
  }
  
  return valid;
}

// Fill the global CPak info. This includes parsing
// the save game note names and the corresponding game
// IDs.
void readEntries( uint8_t* arr ) {
  curCPakInfo.nrEntries = 0;
  if ( !validHeader( arr ) ) {
    curCPakInfo.nrEntries = -1;
    return;
  }
 
  // We only read the game note area, which is placed at pages 3 and 4.
  // As a page is 256B large, we go from 0x300 to 0x500.
  // Each note information is 32B long.
  for ( int i = 0x300; i < 0x500; i += 32 ) {
    // Get the I-Node for a validity check.
    uint16_t inode = ( arr[ i + 0x06 ] << 8 ) + arr[ i + 0x07 ];
    // The first valid game page is 5, the last is 127.
    bool val = ( inode >= 5 && inode <= 127 );
    if ( val ) {
      char* name = curCPakInfo.entries[ curCPakInfo.nrEntries ].entryName;
      unsigned int ind = 0;
      // The note's name is 16B long at an offset of 0x10.
      for ( int j = 0; j < 16; ++j ) {
        name[ ind++ ] = getChar( arr[ i + 0x10 + j ] );
      }
      
      // For now, just ignore the extensions.
      name[ ind ] = '\0';

      // The first four bytes are the game ID.
      char c0, c1, c2, c3;
      c0 = (char) arr[ i ];
      c1 = (char) arr[ i + 1 ];
      c2 = (char) arr[ i + 2 ];
      c3 = (char) arr[ i + 3 ];

      Serial.print( "Gamecode: " );
      Serial.print( c0 );
      Serial.print( c1 );
      Serial.print( c2 );
      Serial.println( c3 );

      curCPakInfo.entries[ curCPakInfo.nrEntries ].gameID = getGameTitle( c0, c1, c2 );
      curCPakInfo.entries[ curCPakInfo.nrEntries ].region = c3;
      
      curCPakInfo.nrEntries++;
    }
  }
  
  return;
}


// A very beautiful function to get the game title.
const char* getGameTitle( char c0, char c1, char c2 ) {
  switch ( c0 ) {
    case 'B':
      switch ( c1 ) {
        case 'S':
          switch ( c2 ) {
            case 'X':
              return title1;
            default:
              return title0;}
        default:
          return title0;}
    case 'C':
      switch ( c1 ) {
        case 'D':
          switch ( c2 ) {
            case 'Z':
              return title2;
            default:
              return title0;}
        case 'F':
          switch ( c2 ) {
            case 'Z':
              return title3;
            default:
              return title0;}
        case 'L':
          switch ( c2 ) {
            case 'B':
              return title4;
            default:
              return title0;}
        case 'P':
          switch ( c2 ) {
            case '2':
              return title5;
            case 'S':
              return title6;
            default:
              return title0;}
        case 'Z':
          switch ( c2 ) {
            case 'G':
              return title7;
            case 'L':
              return title8;
            default:
              return title0;}
        default:
          return title0;}
    case 'F':
      switch ( c1 ) {
        case '7':
          switch ( c2 ) {
            case 'I':
              return title9;
            default:
              return title0;}
        default:
          return title0;}
    case 'N':
      switch ( c1 ) {
        case '0':
          switch ( c2 ) {
            case 'H':
              return title10;
            default:
              return title0;}
        case '2':
          switch ( c2 ) {
            case '2':
              return title11;
            case 'M':
              return title12;
            case 'P':
              return title13;
            case 'V':
              return title14;
            default:
              return title0;}
        case '3':
          switch ( c2 ) {
            case '2':
              return title15;
            case 'D':
              return title16;
            case 'H':
              return title17;
            case 'P':
              return title18;
            case 'T':
              return title19;
            default:
              return title0;}
        case '7':
          switch ( c2 ) {
            case 'I':
              return title20;
            default:
              return title0;}
        case '8':
          switch ( c2 ) {
            case 'I':
              return title21;
            case 'M':
              return title22;
            case 'W':
              return title23;
            default:
              return title0;}
        case '9':
          switch ( c2 ) {
            case 'B':
              return title24;
            case 'C':
              return title25;
            case 'F':
              return title26;
            case 'M':
              return title27;
            default:
              return title0;}
        case 'A':
          switch ( c2 ) {
            case '2':
              return title28;
            case 'B':
              return title29;
            case 'C':
              return title30;
            case 'D':
              return title31;
            case 'F':
              return title32;
            case 'G':
              return title33;
            case 'H':
              return title34;
            case 'I':
              return title35;
            case 'L':
              return title36;
            case 'M':
              return title37;
            case 'R':
              return title38;
            case 'S':
              return title39;
            case 'Y':
              return title40;
            default:
              return title0;}
        case 'B':
          switch ( c2 ) {
            case '2':
              return title41;
            case '3':
              return title42;
            case '4':
              return title43;
            case '5':
              return title44;
            case '6':
              return title45;
            case '7':
              return title46;
            case '9':
              return title47;
            case 'A':
              return title48;
            case 'C':
              return title49;
            case 'D':
              return title50;
            case 'E':
              return title51;
            case 'F':
              return title52;
            case 'H':
              return title53;
            case 'I':
              return title54;
            case 'J':
              return title55;
            case 'K':
              return title56;
            case 'L':
              return title57;
            case 'M':
              return title58;
            case 'N':
              return title59;
            case 'O':
              return title60;
            case 'P':
              return title61;
            case 'Q':
              return title62;
            case 'R':
              return title63;
            case 'S':
              return title64;
            case 'U':
              return title65;
            case 'V':
              return title66;
            case 'W':
              return title67;
            case 'X':
              return title68;
            case 'Y':
              return title69;
            case 'Z':
              return title70;
            default:
              return title0;}
        case 'C':
          switch ( c2 ) {
            case '2':
              return title71;
            case 'B':
              return title72;
            case 'C':
              return title73;
            case 'D':
              return title74;
            case 'E':
              return title75;
            case 'F':
              return title76;
            case 'G':
              return title77;
            case 'H':
              return title78;
            case 'K':
              return title79;
            case 'L':
              return title80;
            case 'O':
              return title81;
            case 'R':
              return title82;
            case 'S':
              return title83;
            case 'T':
              return title84;
            case 'U':
              return title85;
            case 'W':
              return title86;
            case 'X':
              return title87;
            case 'Y':
              return title88;
            case 'Z':
              return title89;
            default:
              return title0;}
        case 'D':
          switch ( c2 ) {
            case '2':
              return title90;
            case '3':
              return title91;
            case '4':
              return title92;
            case '6':
              return title93;
            case 'A':
              return title94;
            case 'C':
              return title95;
            case 'E':
              return title96;
            case 'F':
              return title97;
            case 'G':
              return title98;
            case 'H':
              return title99;
            case 'K':
              return title100;
            case 'M':
              return title101;
            case 'N':
              return title102;
            case 'O':
              return title103;
            case 'Q':
              return title104;
            case 'R':
              return title105;
            case 'S':
              return title106;
            case 'T':
              return title107;
            case 'U':
              return title108;
            case 'W':
              return title109;
            case 'Y':
              return title110;
            case 'Z':
              return title111;
            default:
              return title0;}
        case 'E':
          switch ( c2 ) {
            case 'A':
              return title112;
            case 'G':
              return title113;
            case 'L':
              return title114;
            case 'N':
              return title115;
            case 'P':
              return title116;
            case 'R':
              return title117;
            case 'T':
              return title118;
            case 'V':
              return title119;
            default:
              return title0;}
        case 'F':
          switch ( c2 ) {
            case '0':
              return title120;
            case '2':
              return title121;
            case '9':
              return title122;
            case 'B':
              return title123;
            case 'D':
              return title124;
            case 'F':
              return title125;
            case 'G':
              return title126;
            case 'H':
              return title127;
            case 'L':
              return title128;
            case 'N':
              return title129;
            case 'P':
              return title130;
            case 'Q':
              return title131;
            case 'R':
              return title132;
            case 'S':
              return title133;
            case 'U':
              return title134;
            case 'W':
              return title135;
            case 'X':
              return title136;
            case 'Y':
              return title137;
            case 'Z':
              return title138;
            default:
              return title0;}
        case 'G':
          switch ( c2 ) {
            case '2':
              return title139;
            case '5':
              return title140;
            case '6':
              return title141;
            case 'A':
              return title142;
            case 'B':
              return title143;
            case 'C':
              return title144;
            case 'D':
              return title145;
            case 'E':
              return title146;
            case 'L':
              return title147;
            case 'M':
              return title148;
            case 'N':
              return title149;
            case 'P':
              return title150;
            case 'R':
              return title151;
            case 'S':
              return title152;
            case 'T':
              return title153;
            case 'U':
              return title154;
            case 'V':
              return title155;
            case 'X':
              return title156;
            default:
              return title0;}
        case 'H':
          switch ( c2 ) {
            case '5':
              return title157;
            case '9':
              return title158;
            case 'A':
              return title159;
            case 'B':
              return title160;
            case 'C':
              return title161;
            case 'F':
              return title162;
            case 'G':
              return title163;
            case 'K':
              return title164;
            case 'L':
              return title165;
            case 'M':
              return title166;
            case 'N':
              return title167;
            case 'O':
              return title168;
            case 'P':
              return title169;
            case 'S':
              return title170;
            case 'T':
              return title171;
            case 'V':
              return title172;
            case 'W':
              return title173;
            case 'X':
              return title174;
            case 'Y':
              return title175;
            default:
              return title0;}
        case 'I':
          switch ( c2 ) {
            case 'B':
              return title176;
            case 'C':
              return title177;
            case 'J':
              return title178;
            case 'M':
              return title179;
            case 'R':
              return title180;
            case 'S':
              return title181;
            case 'V':
              return title182;
            default:
              return title0;}
        case 'J':
          switch ( c2 ) {
            case '2':
              return title183;
            case '5':
              return title184;
            case 'A':
              return title185;
            case 'E':
              return title186;
            case 'F':
              return title187;
            case 'M':
              return title188;
            case 'O':
              return title189;
            case 'P':
              return title190;
            case 'Q':
              return title191;
            default:
              return title0;}
        case 'K':
          switch ( c2 ) {
            case '2':
              return title192;
            case '4':
              return title193;
            case 'A':
              return title194;
            case 'E':
              return title195;
            case 'G':
              return title196;
            case 'I':
              return title197;
            case 'J':
              return title198;
            case 'K':
              return title199;
            case 'Q':
              return title200;
            case 'R':
              return title201;
            case 'T':
              return title202;
            default:
              return title0;}
        case 'L':
          switch ( c2 ) {
            case '2':
              return title203;
            case 'B':
              return title204;
            case 'C':
              return title205;
            case 'G':
              return title206;
            case 'L':
              return title207;
            case 'R':
              return title208;
            default:
              return title0;}
        case 'M':
          switch ( c2 ) {
            case '3':
              return title209;
            case '4':
              return title210;
            case '6':
              return title211;
            case '8':
              return title212;
            case '9':
              return title213;
            case 'B':
              return title214;
            case 'D':
              return title215;
            case 'E':
              return title216;
            case 'F':
              return title217;
            case 'G':
              return title218;
            case 'H':
              return title219;
            case 'I':
              return title220;
            case 'J':
              return title221;
            case 'K':
              return title222;
            case 'L':
              return title223;
            case 'M':
              return title224;
            case 'O':
              return title225;
            case 'P':
              return title226;
            case 'Q':
              return title227;
            case 'R':
              return title228;
            case 'S':
              return title229;
            case 'T':
              return title230;
            case 'U':
              return title231;
            case 'V':
              return title232;
            case 'W':
              return title233;
            case 'X':
              return title234;
            case 'Y':
              return title235;
            case 'Z':
              return title236;
            default:
              return title0;}
        case 'N':
          switch ( c2 ) {
            case '2':
              return title237;
            case '6':
              return title238;
            case '9':
              return title239;
            case 'A':
              return title240;
            case 'B':
              return title241;
            case 'C':
              return title242;
            case 'G':
              return title243;
            case 'L':
              return title244;
            case 'M':
              return title245;
            case 'S':
              return title246;
            default:
              return title0;}
        case 'O':
          switch ( c2 ) {
            case '2':
              return title247;
            case '7':
              return title248;
            case 'B':
              return title249;
            case 'F':
              return title250;
            case 'M':
              return title251;
            case 'S':
              return title252;
            case 'W':
              return title253;
            default:
              return title0;}
        case 'P':
          switch ( c2 ) {
            case '2':
              return title254;
            case '3':
              return title255;
            case '6':
              return title256;
            case '9':
              return title257;
            case 'D':
              return title258;
            case 'F':
              return title259;
            case 'G':
              return title260;
            case 'K':
              return title261;
            case 'L':
              return title262;
            case 'N':
              return title263;
            case 'O':
              return title264;
            case 'P':
              return title265;
            case 'Q':
              return title266;
            case 'R':
              return title267;
            case 'T':
              return title268;
            case 'U':
              return title269;
            case 'W':
              return title270;
            case 'X':
              return title271;
            case 'Z':
              return title272;
            default:
              return title0;}
        case 'Q':
          switch ( c2 ) {
            case '2':
              return title273;
            case '8':
              return title274;
            case '9':
              return title275;
            case 'B':
              return title276;
            case 'C':
              return title277;
            case 'K':
              return title278;
            default:
              return title0;}
        case 'R':
          switch ( c2 ) {
            case '2':
              return title279;
            case '3':
              return title280;
            case '6':
              return title281;
            case '7':
              return title282;
            case 'A':
              return title283;
            case 'C':
              return title284;
            case 'D':
              return title285;
            case 'E':
              return title286;
            case 'G':
              return title287;
            case 'H':
              return title288;
            case 'I':
              return title289;
            case 'K':
              return title290;
            case 'O':
              return title291;
            case 'P':
              return title292;
            case 'R':
              return title293;
            case 'S':
              return title294;
            case 'T':
              return title295;
            case 'U':
              return title296;
            case 'V':
              return title297;
            case 'W':
              return title298;
            case 'X':
              return title299;
            case 'Z':
              return title300;
            default:
              return title0;}
        case 'S':
          switch ( c2 ) {
            case '2':
              return title301;
            case '3':
              return title302;
            case '4':
              return title303;
            case '6':
              return title304;
            case 'A':
              return title305;
            case 'B':
              return title306;
            case 'C':
              return title307;
            case 'D':
              return title308;
            case 'F':
              return title309;
            case 'G':
              return title310;
            case 'H':
              return title311;
            case 'I':
              return title312;
            case 'K':
              return title313;
            case 'L':
              return title314;
            case 'M':
              return title315;
            case 'O':
              return title316;
            case 'P':
              return title317;
            case 'Q':
              return title318;
            case 'S':
              return title319;
            case 'T':
              return title320;
            case 'U':
              return title321;
            case 'V':
              return title322;
            case 'W':
              return title323;
            case 'X':
              return title324;
            case 'Y':
              return title325;
            case 'Z':
              return title326;
            default:
              return title0;}
        case 'T':
          switch ( c2 ) {
            case '2':
              return title327;
            case '4':
              return title328;
            case '6':
              return title329;
            case '9':
              return title330;
            case 'A':
              return title331;
            case 'B':
              return title332;
            case 'C':
              return title333;
            case 'E':
              return title334;
            case 'F':
              return title335;
            case 'H':
              return title336;
            case 'I':
              return title337;
            case 'J':
              return title338;
            case 'K':
              return title339;
            case 'M':
              return title340;
            case 'N':
              return title341;
            case 'O':
              return title342;
            case 'P':
              return title343;
            case 'Q':
              return title344;
            case 'R':
              return title345;
            case 'S':
              return title346;
            case 'T':
              return title347;
            case 'U':
              return title348;
            case 'W':
              return title349;
            case 'X':
              return title350;
            default:
              return title0;}
        case 'V':
          switch ( c2 ) {
            case '2':
              return title351;
            case '3':
              return title352;
            case '8':
              return title353;
            case 'B':
              return title354;
            case 'C':
              return title355;
            case 'G':
              return title356;
            case 'L':
              return title357;
            case 'P':
              return title358;
            case 'R':
              return title359;
            default:
              return title0;}
        case 'W':
          switch ( c2 ) {
            case '2':
              return title360;
            case '3':
              return title361;
            case '4':
              return title362;
            case '8':
              return title363;
            case 'A':
              return title364;
            case 'B':
              return title365;
            case 'C':
              return title366;
            case 'D':
              return title367;
            case 'F':
              return title368;
            case 'G':
              return title369;
            case 'I':
              return title370;
            case 'K':
              return title371;
            case 'L':
              return title372;
            case 'M':
              return title373;
            case 'N':
              return title374;
            case 'O':
              return title375;
            case 'P':
              return title376;
            case 'Q':
              return title377;
            case 'R':
              return title378;
            case 'S':
              return title379;
            case 'T':
              return title380;
            case 'U':
              return title381;
            case 'V':
              return title382;
            case 'W':
              return title383;
            case 'X':
              return title384;
            case 'Z':
              return title385;
            default:
              return title0;}
        case 'X':
          switch ( c2 ) {
            case '2':
              return title386;
            case '3':
              return title387;
            case 'F':
              return title388;
            case 'G':
              return title389;
            case 'O':
              return title390;
            default:
              return title0;}
        case 'Y':
          switch ( c2 ) {
            case '2':
              return title391;
            case 'K':
              return title392;
            case 'P':
              return title393;
            case 'S':
              return title394;
            case 'W':
              return title395;
            default:
              return title0;}
        case 'Z':
          switch ( c2 ) {
            case 'L':
              return title396;
            case 'O':
              return title397;
            case 'S':
              return title398;
            default:
              return title0;}
        default:
          return title0;}
    case 'P':
      switch ( c1 ) {
        case 'E':
          switch ( c2 ) {
            case 'P':
              return title399;
            default:
              return title0;}
        default:
          return title0;}
    default:
      return title0;
    }
}
