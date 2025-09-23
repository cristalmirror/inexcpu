#include <iostream>

using namespace std;

int main (int argc, char *argv[]) {

  switch (argv[1]) {
  case "-a": cout<<"manual"<<endl; break;
  default: cout<<"nothing"<<endl; break;
  }
  
  return 0;
}
