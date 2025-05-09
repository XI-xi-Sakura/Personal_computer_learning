#include "Protocol.hpp"

int main()
{
    Request req(10, 20, '+');
    std::string s;
    req.Serialize(s);
    std::cout << s << std::endl;


    req.Deserialize(s);
    req.Print();
    return 0;
}