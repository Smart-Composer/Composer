#include "TestProcess.h"

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    composer::tests::reportFailuresWithoutDialogs();
    return Catch::Session().run(argc, argv);
}
