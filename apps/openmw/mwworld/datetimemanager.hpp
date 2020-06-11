#ifndef GAME_MWWORLD_DATETIMEMANAGER_H
#define GAME_MWWORLD_DATETIMEMANAGER_H

#include <string>

namespace ESM
{
    struct EpochTimeStamp;
}

namespace MWWorld
{
    class Globals;
    class TimeStamp;

    class DateTimeManager
    {
        int mDaysPassed = 0;
        int mDay = 0;
        int mMonth = 0;
        int mYear = 0;
        float mGameHour = 0.f;
        float mTimeScale = 0.f;

        void setHour(double hour);
        void setDay(int day);
        void setMonth(int month);

        /*
            Start of tes3mp addition

            Make it possible to set the year from elsewhere
        */
        void setYear(int year);
        /*
            End of tes3mp addition
        */

        /*
            Start of tes3mp addition

            Make it possible to set the number of days passed from elsewhere
        */
        void setDaysPassed(int daysPassed);
        /*
            End of tes3mp addition
        */

        /*
            Start of tes3mp addition

            Make it possible to set a custom timeScale from elsewhere
        */
        void setTimeScale(float timeScale);
        /*
            End of tes3mp addition
        */

    public:
        std::string getMonthName(int month) const;
        TimeStamp getTimeStamp() const;
        ESM::EpochTimeStamp getEpochTimeStamp() const;
        float getTimeScaleFactor() const;

        void advanceTime(double hours, Globals& globalVariables);

        void setup(Globals& globalVariables);
        bool updateGlobalInt(const std::string& name, int value);
        bool updateGlobalFloat(const std::string& name, float value);
    };
}

#endif
