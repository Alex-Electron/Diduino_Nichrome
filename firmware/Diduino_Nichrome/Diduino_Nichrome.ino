//===========================================================================
//  DIDUINO NICHROME  v0.7.1
//  Programmer firmware for Soviet bipolar (fusible-link) PROMs:
//    КР556РТ4 (256x4)  and  К155РЕ3 (32x8).  Select with H0 (РТ4, default) / H1 (РЕ3).
//  Arduino Nano (ATmega328P) + boost board "DID_PROG" (rev 1.1/1.2).
//
//  Burn protocol: host streams chip.words bytes; firmware checksum-gates the transfer,
//  programs with verify-per-bit, then re-reads & verifies the whole chip on-board.
//  Serial 115200 8N1. Commands (Enter-terminated): H<n> · v · p<n> · I/L/D/S · R · B · T · j · ?
//  Also a diagnostic set for board bring-up: a/c/m/f/g/e/k/W.  (no command uses both letter cases)
//
//  Based on the Diduino programmer (BSD-2-Clause):
//    original (Radionews): https://github.com/Radionews/diduino
//    base (Walhi):         https://github.com/walhi/arduino_eprom27_programmer
//    board (EasyEDA):      https://oshwlab.com/naym1993/prog_did
//
//  Author:    Alexander Lavrinovich  (github.com/Alex-Electron)
//  E-mail:    lavrinovich.alex@gmail.com
//  Co-author: AI
//  License:   BSD-2-Clause  (inherited from the original Diduino)
//===========================================================================

#include <EEPROM.h>    // stores the one-time Vpp calibration constant

#define ST A3          // address shift-reg CLOCK (shiftOut clock pin)
#define SH A2          // address shift-reg LATCH (RCLK), pulsed low->high
#define DS A1          // address shift-reg DATA
#define AR0 2          // data-bit mux select bit0 (DG408 A0)
#define AR1 3          // data-bit mux select bit1 (DG408 A1)
#define AR2 4          // data-bit mux select bit2 (DG408 A2)
#define READ 5         // data read-back input (sense)
#define CS 6           // chip-select drive
#define WRITE 7        // program-pulse drive (HIGH = Vpp applied via Q4)
#define DS_POWER 8      // DS18B20 power (unused here, kept LOW)
#define LED 13
#define FW_NAME "DIDUINO NICHROME"
#define FW_VERSION "0.7.1"

// programming-voltage feedback shift register (boost level), pins per original
#define PWR_DATA 10
#define PWR_LATCH 11
#define PWR_CLK 12
#define PWR_A0 A0

// internal voltmeter (Vpp sense divider on A4), RATIOMETRIC against the internal 1.1V bandgap:
//   Vpp = (adcVpp / adcBandgap) * vCal   — Vcc cancels, so the reading is correct on ANY supply
//   (external/7805/USB) and immune to sag. vCal absorbs the divider ratio and the per-chip bandgap
//   tolerance; default works out of the box (~±10%), 'T<centivolts>' calibrates it exactly into EEPROM.
#define voltageControl A4
#define rTop 10000.0
#define rBottom 2200.0
#define VBG_NOMINAL 1.1                                    // ATmega328P internal bandgap, nominal (V)
#define VCAL_DEFAULT (VBG_NOMINAL*(rTop+rBottom)/rBottom)  // ≈ 6.10 = 1.1 × divider ratio
#define EE_CAL_ADDR 0
#define EE_CAL_MAGIC 0xC1A5
float vCal = VCAL_DEFAULT;                                 // live constant; loaded from EEPROM if calibrated

word shift_data = 0;    // current 16-bit address word held on the bus
uint8_t curLevel = 0;   // current Vpp level 0..7
uint8_t curMux = 0;     // current data-bit mux 0..7
uint16_t prog_imp = 1000;   // BURN: max pulses per bit
uint16_t prog_len = 40;     // BURN: pulse width, us
uint8_t  prog_duty = 10;    // BURN: duty % (recovery = len*(100/duty) us)
uint16_t prog_soak = 50;    // BURN: extra over-program (soak) pulses after a bit takes

//--- chip model -----------------------------------------------------------
// Everything chip-specific lives here so the read/blank/burn/verify paths stay generic.
// Verified against the original Diduino (cs_set/read_byte/write_byte): РЕ3 differs from РТ4
// ONLY in width (8 vs 4 bits) and size (32 vs 256 words). CS polarity, inverted sense, the
// 0->1 blow direction and the pulse mechanics are identical and inherited from the РТ4 path.
struct ChipDef { uint16_t words; uint8_t bits; uint8_t vpp; const char* name; };
const ChipDef CHIP_RT4 = {256, 4, 1, "RT4"};   // КР556РТ4 256x4, program at p1 (~12.5V)
const ChipDef CHIP_RE3 = {32,  8, 0, "RE3"};   // К155РЕ3  32x8,  program at p0 (~11.6V); РЕ3 likes a lower, adjustable Vpp
ChipDef chip = CHIP_RT4;                        // current selection (default РТ4 = previous behavior)
#define DMASK ((byte)((1u<<chip.bits)-1))       // data mask: 0x0F for РТ4, 0xFF for РЕ3

//--- low-level (verbatim logic from original firmware) ---------------------
void init_power(){
  pinMode(PWR_CLK, OUTPUT);
  pinMode(PWR_LATCH, OUTPUT);
  pinMode(PWR_DATA, OUTPUT);
  pinMode(PWR_A0, OUTPUT);
  digitalWrite(PWR_A0, HIGH);   // level 0 = base voltage
}
void set_power(uint8_t level){
  if(level>7) level = 7;
  curLevel = level;
  if(level==0){ digitalWrite(PWR_A0, HIGH); }
  else{
    digitalWrite(PWR_LATCH, LOW);
    shiftOut(PWR_DATA, PWR_CLK, MSBFIRST, ~(1<<(7-level)));
    digitalWrite(PWR_LATCH, HIGH);
    digitalWrite(PWR_A0, LOW);
  }
}
void load_shift(){
  digitalWrite(SH, LOW);
  shiftOut(DS, ST, LSBFIRST, (shift_data>>8)&0xFF);
  shiftOut(DS, ST, LSBFIRST, shift_data&0xFF);
  digitalWrite(SH, HIGH);
}
void set_mux(uint8_t m){
  curMux = m & 0x07;
  digitalWrite(AR0, (curMux>>0)&1);
  digitalWrite(AR1, (curMux>>1)&1);
  digitalWrite(AR2, (curMux>>2)&1);
}
// Read the data word at the CURRENT address (shift_data), chip.bits wide, inverted sense
// (logical 1 = READ pin low) — exactly the per-bit sequence the original used, factored out
// of the 5 places that duplicated it. Caller sets shift_data first.
byte readData(){
  byte out=0;
  for(uint8_t j=0;j<chip.bits;j++){ set_mux(j); load_shift(); delayMicroseconds(40); out|=(!digitalRead(READ))<<j; }
  return out;
}
// one averaged 10-bit ADC reading on channel `mux`, reference = AVcc (REFS0)
uint16_t adcRead(uint8_t mux){
  ADMUX = _BV(REFS0) | (mux & 0x0F);
  delay(2);                                                  // let input/ref settle (bandgap needs it)
  ADCSRA |= _BV(ADSC); while(ADCSRA & _BV(ADSC)); (void)ADC; // discard first conversion after the switch
  uint32_t s=0; for(uint8_t i=0;i<16;i++){ ADCSRA |= _BV(ADSC); while(ADCSRA & _BV(ADSC)); s+=ADC; }
  return (uint16_t)(s>>4);
}
void sampleVpp(uint16_t &aVpp, uint16_t &aBg){ aVpp=adcRead(4); aBg=adcRead(0x0E); }  // A4 + 1.1V bandgap (ch 0x0E)
float get_voltage(){
  uint16_t aVpp,aBg; sampleVpp(aVpp,aBg);
  if(!aBg) return 0;                                         // guard
  return ((float)aVpp/(float)aBg)*vCal;                      // ratiometric → supply-independent
}
// EEPROM-persisted calibration (magic + float), survives power cycles and any later supply change
void calLoad(){ uint16_t m; EEPROM.get(EE_CAL_ADDR,m);
  if(m==EE_CAL_MAGIC){ float c; EEPROM.get(EE_CAL_ADDR+2,c); if(c>0.5 && c<50) vCal=c; } }
void calSave(float c){ uint16_t m=EE_CAL_MAGIC; EEPROM.put(EE_CAL_ADDR,m); EEPROM.put(EE_CAL_ADDR+2,c); }
void calReset(){ uint16_t m=0xFFFF; EEPROM.put(EE_CAL_ADDR,m); vCal=VCAL_DEFAULT; }

//--- helpers ---------------------------------------------------------------
long readNum(String s){            // parse decimal or 0x.. hex after the cmd char
  s.trim();
  if(s.length()==0) return -1;
  if(s.startsWith("0x")||s.startsWith("0X")) return strtol(s.c_str(),NULL,16);
  return s.toInt();
}
void safeState(){
  digitalWrite(WRITE, LOW);        // no program pulse
  digitalWrite(CS, HIGH);          // CS to its read-state level
  set_power(0);                    // base Vpp
  shift_data = 0; load_shift();    // address 0
  set_mux(0);
}
void endBurn(){                    // serial hygiene after a burn: restore timeout, drain leftover stream bytes
  Serial.setTimeout(1000);         // restore default (raised to 10000 for the 256-byte image read)
  delay(20);                       // let any in-flight tail bytes arrive
  while(Serial.available()) Serial.read();
}

void help(){
  Serial.println(F("--- " FW_NAME " v" FW_VERSION " ---"));
  Serial.println(F("a<n>  set & HOLD address (dec or 0xHEX), CS=read-state"));
  Serial.println(F("c<0|1> CS pin level"));
  Serial.println(F("p<0-7> Vpp/boost level (RAISES voltage! socket must be EMPTY)"));
  Serial.println(F("m<0-7> data-bit mux select (which data line routed)"));
  Serial.println(F("f     read current READ pin (raw + logical)"));
  Serial.println(F("g     read all data bits at current address (chip.bits wide)"));
  Serial.println(F("e<ms> energize Vpp on selected data bit for ms then release"));
  Serial.println(F("k     walk: each address line HIGH one-by-one (scope), 800ms each"));
  Serial.println(F("v     measure Vpp (internal voltmeter)"));
  Serial.println(F("T<cV> calibrate Vpp, centivolts e.g.1250=12.50V (EEPROM); T0 reset; T query"));
  Serial.println(F("j     SAFE state (addr0, CS read, WRITE low, level0)"));
  Serial.println(F("H<n>  select chip: H0=КР556РТ4 (256x4), H1=К155РЕ3 (32x8); H=query"));
  Serial.println(F("?     this help"));
  // note: no command uses both letter cases — upper/lower never collide (e.g. R/r, D/d, S/s are split apart)
}

void setup(){
  init_power();
  pinMode(ST,OUTPUT); pinMode(SH,OUTPUT); pinMode(DS,OUTPUT);
  pinMode(WRITE,OUTPUT); digitalWrite(WRITE,LOW);
  pinMode(CS,OUTPUT);    digitalWrite(CS,HIGH);
  pinMode(DS_POWER,OUTPUT); digitalWrite(DS_POWER,LOW);
  pinMode(READ,INPUT);
  pinMode(AR0,OUTPUT); pinMode(AR1,OUTPUT); pinMode(AR2,OUTPUT);
  pinMode(LED,OUTPUT); digitalWrite(LED,LOW);
  safeState();
  calLoad();                       // restore Vpp calibration (if any) before first reading
  Serial.begin(115200);
  Serial.println(F(FW_NAME " v" FW_VERSION " (115200). Type ? for help."));
}

void loop(){
  if(!Serial.available()) return;
  char c = Serial.read();
  if(c=='\n'||c=='\r'||c==' ') return;
  String arg = Serial.readStringUntil('\n');
  long n = readNum(arg);
  switch(c){
    case 'a':
      if(n<0) n=0;
      shift_data = (word)n;
      digitalWrite(CS,HIGH);        // read-state so address buffers active
      load_shift();
      Serial.print(F("ADDR=0x")); Serial.println(shift_data, HEX);
      break;
    case 'c':
      digitalWrite(CS, n?HIGH:LOW);
      Serial.print(F("CS pin=")); Serial.println(n?F("HIGH"):F("LOW"));
      break;
    case 'p':
      set_power((uint8_t)n);
      delay(300);
      Serial.print(F("LEVEL=")); Serial.print(curLevel);
      Serial.print(F("  Vpp=")); Serial.println(get_voltage(),2);
      break;
    case 'm':
      set_mux((uint8_t)n);
      Serial.print(F("MUX=")); Serial.println(curMux);
      break;
    case 'f':{                       // (was 'r'; split from 'R'=read-chip to avoid a case-pair)
      int raw = digitalRead(READ);
      Serial.print(F("READ raw=")); Serial.print(raw);
      Serial.print(F("  logical=")); Serial.println(!raw);
      break; }
    case 'g':{                       // (was 'd'; split from 'D'=duty to avoid a case-pair)
      Serial.print(F("DATA bits @0x")); Serial.print(shift_data,HEX); Serial.print(F(": "));
      byte out=0;
      for(uint8_t j=0;j<chip.bits;j++){ set_mux(j); delay(2);
        byte b=!digitalRead(READ); out|=b<<j;
        Serial.print(b); Serial.print(' '); }
      Serial.print(F(" = 0x")); Serial.println(out,HEX);
      break; }
    case 'e':{
      if(n<1) n=1000; if(n>5000) n=5000;
      Serial.print(F("Vpp on data bit ")); Serial.print(curMux);
      Serial.print(F(" for ")); Serial.print(n); Serial.println(F(" ms"));
      digitalWrite(CS,LOW); load_shift();
      digitalWrite(WRITE,HIGH);
      delay(n);
      digitalWrite(WRITE,LOW); digitalWrite(CS,HIGH); load_shift();
      Serial.println(F("released"));
      break; }
    case 'k':
      Serial.println(F("address line walk..."));
      for(uint8_t b=0;b<16;b++){ shift_data=(word)1<<b; load_shift();
        Serial.print(F("A")); Serial.print(b); Serial.println(F(" HIGH"));
        digitalWrite(LED,!digitalRead(LED)); delay(800); }
      shift_data=0; load_shift();
      Serial.println(F("walk done"));
      break;
    case 'v':
      Serial.print(F("Vpp=")); Serial.println(get_voltage(),2);
      break;
    case 'T':{
      // Vpp calibration (Trim). T<centivolts> sets vCal so the reading matches the meter (e.g. T1250 = 12.50V),
      // saved in EEPROM (one-time, survives any later supply change). T0 = reset; T = query.
      // Centivolts (integer) on purpose: reuses readNum's toInt and avoids pulling strtod (~0.5KB → fits ATmega168).
      // 'T' chosen because both cases are unused — no risk of a case mix-up firing another command.
      if(n<0){ Serial.print(F("CAL=")); Serial.println(vCal,4); break; }
      if(n==0){ calReset(); Serial.print(F("CAL=")); Serial.println(vCal,4); break; }
      uint16_t aVpp,aBg; sampleVpp(aVpp,aBg);
      if(!aVpp || !aBg){ Serial.println(F("CAL ERR: set a level first")); break; }
      float c = (n/100.0) * (float)aBg / (float)aVpp;        // C = Vpp_meter × adcBg / adcVpp
      if(c < 2.0 || c > 15.0){                               // plausible C ≈ Vbg×divider ≈ 6; reject garbage
        Serial.print(F("CAL ERR: C=")); Serial.print(c,3);   // (e.g. value sent in wrong units → 100x off)
        Serial.println(F(" out of range, not saved")); break; }
      vCal = c; calSave(vCal);
      Serial.print(F("CAL=")); Serial.print(vCal,4);
      Serial.print(F(" Vpp=")); Serial.println(get_voltage(),2);
      break; }
    case 'W':{
      // Program a 4-bit value at ADDRESS 0 (RT4), faithful to original write_byte:
      // 40us pulse / 800us recovery, up to 1000 pulses per bit, early-exit on readback.
      // Set the desired Vpp level with p<n> BEFORE calling this. Socket must hold a chip.
      byte val = (byte)n & DMASK;
      shift_data = 0;
      Serial.print(F("PROGRAM 0x")); Serial.print(val,HEX); Serial.println(F(" @ addr 0"));
      for(uint8_t j=0;j<chip.bits;j++){
        set_mux(j);
        digitalWrite(CS,HIGH); load_shift(); delayMicroseconds(50);
        byte want=(val>>j)&1; byte cur=!digitalRead(READ); int used=0;
        if(want==1 && cur==0){
          for(int k=0;k<1000;k++){
            digitalWrite(CS,LOW); load_shift();
            digitalWrite(WRITE,HIGH); delayMicroseconds(40); digitalWrite(WRITE,LOW);
            digitalWrite(CS,HIGH); load_shift();
            delayMicroseconds(800);
            used=k+1;
            if((!digitalRead(READ))==1) break;
          }
        }
        Serial.print(F(" bit")); Serial.print(j); Serial.print(F(" want=")); Serial.print(want);
        Serial.print(F(" pulses=")); Serial.println(used);
      }
      digitalWrite(WRITE,LOW); digitalWrite(CS,HIGH);
      byte out=0;
      for(uint8_t j=0;j<chip.bits;j++){ set_mux(j); load_shift(); delayMicroseconds(50); out|=(!digitalRead(READ))<<j; }
      Serial.print(F("READBACK @0 = 0x")); Serial.println(out,HEX);
      set_power(0);                  // drop the boost rail after the diagnostic — don't leave Vpp hot
      break; }
    case 'B':{
      // Burn the current chip's image. Usage: send "B\n" then exactly chip.words raw bytes
      // (РТ4: 256, low nibble used; РЕ3: 32, full byte). DMASK masks per chip.
      // Set the level with p<n> BEFORE (floored at the chip default: РТ4 p1, РЕ3 p0). Socket must hold a FRESH blank chip.
      static byte img[256];   // sized for the largest chip (РТ4); РЕ3 uses the first 32
      Serial.print(F("SEND ")); Serial.print(chip.words); Serial.println(F(" BYTES NOW..."));
      Serial.setTimeout(10000);
      int got = Serial.readBytes(img, chip.words);
      if(got != chip.words){ Serial.print(F("ERR: got ")); Serial.println(got); safeState(); endBurn(); break; }
      // transfer-integrity CRC-32 (standard, matches HEX panel + external utilities); host gates burn on it
      uint32_t rxcrc=0xFFFFFFFFUL;
      for(int i=0;i<chip.words;i++){ rxcrc^=img[i]; for(uint8_t k=0;k<8;k++){ if(rxcrc&1) rxcrc=(rxcrc>>1)^0xEDB88320UL; else rxcrc>>=1; } }
      rxcrc=~rxcrc;
      Serial.print(F("RXCRC=0x")); Serial.println(rxcrc, HEX);
      while(Serial.available()) Serial.read();        // flush stale/pipelined bytes; the gate must reflect a post-CRC decision
      // GATE: host must reply 'Y' + 8 hex chars (its expected CRC-32) within 15s. Firmware refuses to burn unless it matches rxcrc.
      { unsigned long t0=millis(); char go=0; char hxs[9]; uint8_t hn=0;
        while(millis()-t0<15000){
          if(Serial.available()){
            char ch=Serial.read();
            if(ch=='N'){ go='N'; break; }
            if(ch=='Y'){ go='Y'; continue; }
            if(go=='Y' && hn<8 && isxdigit(ch)){ hxs[hn++]=ch; if(hn==8){ hxs[8]=0; break; } }
          }
        }
        if(go!='Y'){ Serial.println(F("*** BURN ABORTED ***")); safeState(); endBurn(); break; }
        if(hn!=8){ Serial.println(F("ERR: no expected CRC after Y")); Serial.println(F("*** BURN ABORTED ***")); safeState(); endBurn(); break; }
        uint32_t expect=strtoul(hxs,NULL,16);
        if(expect!=rxcrc){ Serial.print(F("CRC MISMATCH host=0x")); Serial.print(expect,HEX); Serial.print(F(" rx=0x")); Serial.println(rxcrc,HEX);
          Serial.println(F("*** BURN ABORTED ***")); safeState(); endBurn(); break; }
        Serial.println(F("CRC OK")); }
      // PRE-BURN BLANK / COMPATIBILITY CHECK (fuses are one-way: cannot clear a set bit)
      { int bad=0;
        digitalWrite(WRITE,LOW); digitalWrite(CS,HIGH);
        for(int addr=0; addr<chip.words; addr++){
          shift_data=(word)addr;
          byte chipv=readData();
          byte conflict = chipv & (~img[addr]) & DMASK;   // chip has 1 where image wants 0
          if(conflict){ bad++; if(bad<=12){ Serial.print(F(" OVERBURN @0x")); Serial.print(addr,HEX);
            Serial.print(F(" chip=0x")); Serial.print(chipv,HEX); Serial.print(F(" img=0x")); Serial.println(img[addr]&DMASK,HEX); } }
        }
        if(bad){ Serial.print(F("NOT BLANK: ")); Serial.print(bad); Serial.println(F(" conflicting cells"));
          Serial.println(F("*** BURN ABORTED ***")); safeState(); endBurn(); break; }
        Serial.println(F("blank-check OK")); }
      // program at the OPERATOR-selected level, but never BELOW the per-chip default (chip.vpp = floor):
      // the app pre-fills the default (РТ4 p1 / РЕ3 p0) via 'p<n>' and the operator may raise it for stubborn
      // РЕ3 bits (datasheet escalation +0.5V up to ~14V). The floor keeps РТ4 at >=p1 even without the app
      // (a standalone burn can't silently under-volt), while escalation upward stays free.
      uint8_t burnLevel = (curLevel >= chip.vpp) ? curLevel : chip.vpp;
      set_power(burnLevel); delay(300);               // settle the boost at the chosen level
      { float vpp = get_voltage();
        Serial.print(F("Vpp(prog)=")); Serial.println(vpp,2);
        if(vpp < 8.0 || vpp > 17.0){                  // loose gate: catches dead boost / runaway, immune to calibration spread
          Serial.println(F("ERR: Vpp out of range (boost fault?)"));
          Serial.println(F("*** BURN ABORTED ***")); safeState(); endBurn(); break; } }
      long progged = 0;
      for(int addr=0; addr<chip.words; addr++){
        if((addr & 0x0F)==0){ Serial.print(F("PROG ")); Serial.println(addr); }   // progress marker (host moves the bar)
        shift_data = (word)addr;
        byte val = img[addr] & DMASK;
        for(uint8_t j=0;j<chip.bits;j++){
          set_mux(j);
          digitalWrite(CS,HIGH); load_shift(); delayMicroseconds(40);
          byte want=(val>>j)&1; byte cur=!digitalRead(READ);
          if(want==1 && cur==0){
            uint16_t rec = (uint32_t)prog_len*100UL/(prog_duty<1?1:prog_duty); if(rec<prog_len)rec=prog_len; if(rec>16000)rec=16000;
            bool took=false;
            for(int k=0;k<prog_imp;k++){
              digitalWrite(CS,LOW); load_shift();
              digitalWrite(WRITE,HIGH); delayMicroseconds(prog_len); digitalWrite(WRITE,LOW);
              digitalWrite(CS,HIGH); load_shift();
              delayMicroseconds(rec);
              if((!digitalRead(READ))==1){ progged++; took=true; break; }
            }
            for(uint16_t sp=0; took && sp<prog_soak; sp++){   // soak / over-program pulses (clean fuse, anti-regrow)
              digitalWrite(CS,LOW); load_shift();
              digitalWrite(WRITE,HIGH); delayMicroseconds(prog_len); digitalWrite(WRITE,LOW);
              digitalWrite(CS,HIGH); load_shift();
              delayMicroseconds(rec);
            }
          }
        }
      }
      digitalWrite(WRITE,LOW); digitalWrite(CS,HIGH);
      Serial.print(F("BURN done, bits programmed=")); Serial.println(progged);
      // verify at base Vpp: drop the boost to p0 (its floor) for the verify pass. The read path
      // already senses at 5V Vcc with Vpp gated off by WRITE=LOW; this also removes off-state
      // leakage stress and ends the burn with the rail at minimum before the SAFE return.
      set_power(0); delay(300);
      Serial.println(F("Vpp->p0 (verify)"));
      int mism=0;
      for(int addr=0; addr<chip.words; addr++){
        if((addr & 0x0F)==0){ Serial.print(F("VERF ")); Serial.println(addr); }   // progress marker (verify phase)
        shift_data=(word)addr;
        byte out=readData();
        if(out != (img[addr]&DMASK)){
          mism++;
          if(mism<=12){ Serial.print(F(" MISMATCH @0x")); Serial.print(addr,HEX);
            Serial.print(F(" want 0x")); Serial.print(img[addr]&DMASK,HEX);
            Serial.print(F(" got 0x")); Serial.println(out,HEX); }
        }
      }
      Serial.print(F("VERIFY mismatches=")); Serial.println(mism);
      Serial.println(mism==0 ? F("*** BURN OK ***") : F("*** BURN FAILED ***"));
      safeState();                 // always leave the board safe: WRITE low, CS read-state, Vpp->p0
      endBurn();
      break; }
    case 'I': prog_imp=(uint16_t)(n<1?1:n); Serial.print(F("imp=")); Serial.println(prog_imp); break;
    case 'L': prog_len=(uint16_t)(n<1?1:n); Serial.print(F("len=")); Serial.println(prog_len); break;
    case 'D': prog_duty=(uint8_t)(n<1?1:(n>99?99:n)); Serial.print(F("duty=")); Serial.println(prog_duty); break;
    case 'S': prog_soak=(uint16_t)(n<0?0:(n>128?128:n)); Serial.print(F("soak=")); Serial.println(prog_soak); break;
    case 'R':{
      // Read the whole chip. Streams exactly chip.words raw bytes (РТ4: low nibble each; РЕ3: full byte).
      digitalWrite(WRITE,LOW); digitalWrite(CS,HIGH);
      for(int addr=0; addr<chip.words; addr++){
        shift_data=(word)addr;
        Serial.write(readData());
      }
      break; }
    case 'j':                        // (was 's'; split from 'S'=soak to avoid a case-pair)
      safeState();
      Serial.println(F("SAFE state set"));
      break;
    case 'H':{                       // select chip / report. H0=РТ4, H1=РЕ3, H=query. Both letter cases are free.
      if(n==0){ chip=CHIP_RT4; safeState(); }       // size/width changed → return the bus to a known safe state
      else if(n==1){ chip=CHIP_RE3; safeState(); }
      else if(n>1){ Serial.println(F("ERR: H0=RT4 H1=RE3")); break; }   // bad arg
      // n<0 (bare 'H') = pure query: report the current chip WITHOUT disturbing the bus (lets the app probe)
      Serial.print(F("CHIP=")); Serial.print(chip.name);
      Serial.print(' '); Serial.print(chip.words); Serial.print('x'); Serial.println(chip.bits);
      Serial.println(F("CHIPS=RT4,RE3"));   // capability advert: older РТ4-only firmware lacks this line → the app prompts a reflash
      break; }
    case '?': default:
      help();
  }
}
