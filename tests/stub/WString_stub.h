#pragma once
#include <string>
#include <stdint.h>
#include <string.h>
struct String { std::string s; String(){} String(const char*c):s(c?c:""){} String(const std::string&x):s(x){}
  const char* c_str() const {return s.c_str();} unsigned length() const {return s.size();}
  bool operator==(const char*c) const {return s==c;} String operator+(const String&o) const {return String(s+o.s);}
  String operator+(const char*c) const {return String(s+c);}
  void trim(){ size_t a=s.find_first_not_of(" \t\r\n"); size_t b=s.find_last_not_of(" \t\r\n"); s = a==std::string::npos ? "" : s.substr(a,b-a+1); }
  void replace(const char*f,const char*r){ size_t p=0, fl=strlen(f), rl=strlen(r); if(!fl) return; while((p=s.find(f,p))!=std::string::npos){ s.replace(p,fl,r); p+=rl; } } };
inline String operator+(const char*a,const String&b){return String(std::string(a)+b.s);}
struct IPAddress { unsigned char b[4]{0,0,0,0}; IPAddress(){} IPAddress(uint8_t a,uint8_t c,uint8_t e,uint8_t f){b[0]=a;b[1]=c;b[2]=e;b[3]=f;} uint8_t operator[](int i) const {return b[i];} String toString() const { char t[16]; snprintf(t,16,"%u.%u.%u.%u",b[0],b[1],b[2],b[3]); return String(t);} };
#if !defined(__GLIBC__) || !defined(_GNU_SOURCE) || (__GLIBC__ < 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38)
inline size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s); if(n){size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0;} return l;}
inline size_t strlcat(char*d,const char*s,size_t n){size_t dl=strnlen(d,n); if(dl==n) return n+strlen(s); return dl+strlcpy(d+dl,s,n-dl);}
#endif
