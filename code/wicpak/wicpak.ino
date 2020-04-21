#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <ESP8266mDNS.h>

// Some defines.
#define CPAKBYTES 32768
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
char cpakArr[ CPAKBYTES ];

// Flag set high if the CPak should be flashed.
char updateCPak = 0;


// CPak parsing function declarations.
char getChar( uint8_t c );
uint16_t getHeaderChkSum( uint8_t* arr, unsigned int offset );
bool validHeader( uint8_t* arr );
unsigned int readEntries( uint8_t* arr, char entries[][17] );


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
  char gameName[ 62 ];
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

void readMemPak() {
  // Make sure the shiftreg clk is low.
  digitalWrite( SHIFT_CLK_PIN, 0 );

  // Make sure that the data shift reg's output
  // is disabled.
  datRegDisableOut();
  // We go over all addresses.
  for ( unsigned int addr = 0; addr <= MAXADDR; ++addr ) {
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
    cpakArr[ addr ] = curByte;

    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }

  // Store into the flash.
  File f = SPIFFS.open( cpakfile, "w" );
  for ( unsigned int addr = 0; addr <= MAXADDR; ++addr ) {
    f.write( cpakArr[ addr ] );
  }
  f.close();
}

void writeMemPak() {
  // Start by copying the CPAK content from the file
  // to the array.
  File f = SPIFFS.open( cpakfile, "r" );
  // Read in byte by byte.
  if ( f.size() < CPAKBYTES ) {
    Serial.println( "CPak file is shorter than 4096!" );
  }
  for ( int i = 0; i < f.size(); ++i ) {
    if ( i >= CPAKBYTES )
      break;

    cpakArr[ i ] = f.read();
    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }
  f.close();

  Serial.println( "Read CPak from Flash to arra done" );

  // Enable serial shift mode for the data reg.
  datRegShiftMode();
  
  // Write to CPak. Go over all addresses.
  for ( unsigned int addr = 0; addr <= MAXADDR; ++addr ) {
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
      char curDatBit = ( cpakArr[ addr ] & ( 1 << ( 8 - i ) ) );
      // Into the shift register.
      shiftBitBoth( curAddrBit, curDatBit );
    }

    // And the last data bit. This also does the last 
    // clk shift into the output latches of the address reg.
    shiftDataBit( cpakArr[ addr ] & 0b1 );

    // And perform the write.
    FRAMWrite();

    // This could take a while, so feed the dog.
    ESP.wdtFeed();
  }
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
    // Update the CPak info.
    readEntries( (uint8_t*) cpakArr );
    updateCPak = 0;
  }
}

// CPak parsing functions.
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

uint16_t getHeaderChkSum( uint8_t* arr, unsigned int offset ) {
  uint16_t chk = 0;
  for ( unsigned int i = 0; i < 28; i += 2 ) {
    chk += ( arr[ offset + i ] << 8 ) + arr[ offset + i + 1 ];
  }
  
  return chk;
}
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
      curCPakInfo.nrEntries++;
    }
  }
  
  return;
}
