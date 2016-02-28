#include "WateringModule.h"
#include "ModuleController.h"

static uint8_t WATER_RELAYS[] = { WATER_RELAYS_PINS }; // объявляем массив пинов реле

void WateringModule::Setup()
{
  // настройка модуля тут

  controller = GetController();
  settings = controller->GetSettings();

  blinker.begin(DIODE_WATERING_MANUAL_MODE_PIN, F("WM"));  // настраиваем блинкер на нужный пин

  workMode = wwmAutomatic; // автоматический режим работы
  dummyAllChannels.WateringTimer = 0; // обнуляем таймер полива для всех каналов
  dummyAllChannels.IsChannelRelayOn = false; // все реле выключены

  lastDOW = -1; // неизвестный день недели
  currentDOW = -1; // ничего не знаем про текущий день недели
  currentHour = -1; // и про текущий час тоже ничего не знаем

  #ifdef USE_DS3231_REALTIME_CLOCK
    bIsRTClockPresent = true; // есть часы реального времени
  #else
    bIsRTClockPresent = false; // нет часов реального времени
  #endif

   uint8_t relayCnt = WATER_RELAYS_COUNT/8; // устанавливаем кол-во каналов реле
   if(WATER_RELAYS_COUNT > 8 && WATER_RELAYS_COUNT % 8)
    relayCnt++;

   if(WATER_RELAYS_COUNT < 9)
    relayCnt = 1;
    
   for(uint8_t i=0;i<relayCnt;i++) // добавляем состояния реле (каждый канал - 8 реле)
    State.AddState(StateRelay,i);  

    
  // выключаем все реле
  for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
  {
    pinMode(WATER_RELAYS[i],OUTPUT);
    digitalWrite(WATER_RELAYS[i],RELAY_OFF);
  
    uint8_t idx = i/8;
    uint8_t bitNum1 = i % 8;
    OneState* os = State.GetState(StateRelay,idx);
    if(os)
    {
      uint8_t curRelayStates = *((uint8_t*) os->Data);
      bitWrite(curRelayStates,bitNum1, dummyAllChannels.IsChannelRelayOn);
      State.UpdateState(StateRelay,idx,(void*)&curRelayStates);
    }

    // настраиваем все каналы
    wateringChannels[i].IsChannelRelayOn = dummyAllChannels.IsChannelRelayOn;
    wateringChannels[i].WateringTimer = 0;
  } // for

#ifdef USE_PUMP_RELAY
  // выключаем реле насоса  
  pinMode(PUMP_RELAY_PIN,OUTPUT);
  digitalWrite(PUMP_RELAY_PIN,RELAY_OFF);
#endif

    // настраиваем режим работы перед стартом
    WateringOption currentWateringOption = settings->GetWateringOption();
    
    if(currentWateringOption == wateringOFF) // если выключено автоуправление поливом
    {
      workMode = wwmManual; // переходим в ручной режим работы
      blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
    }
    else
    {
      workMode = wwmAutomatic; // иначе переходим в автоматический режим работы
      blinker.blink(); // гасим диод
    }
      

}
void WateringModule::UpdateChannel(int8_t channelIdx, WateringChannel* channel, uint16_t dt)
{
   if(!bIsRTClockPresent)
   {
     // в системе нет модуля часов, в таких условиях мы можем работать только в ручном режиме.
     // поэтому в этой ситуации мы ничего не предпринимаем, поскольку автоматически деградируем
     // в ручной режим работы.
     return;
   }
   
     uint8_t weekDays = channelIdx == -1 ? settings->GetWateringWeekDays() : settings->GetChannelWateringWeekDays(channelIdx);
     uint8_t startWateringTime = channelIdx == -1 ? settings->GetStartWateringTime() : settings->GetChannelStartWateringTime(channelIdx);
     uint16_t timeToWatering = channelIdx == -1 ? settings->GetWateringTime() : settings->GetChannelWateringTime(channelIdx); // время полива (в минутах!)


    // проверяем, установлен ли у нас день недели для полива, и настал ли час, с которого можно поливать
    bool canWork = bitRead(weekDays,currentDOW-1) && currentHour >= startWateringTime;
  
    if(!canWork)
     {            
       channel->WateringTimer = 0; // в этот день недели и в этот час работать не можем, однозначно обнуляем таймер полива    
       channel->IsChannelRelayOn = false; // выключаем реле
     }
     else
     {
      // можем работать, смотрим, не вышли ли мы за пределы установленного интервала
 
      if(lastDOW != currentDOW)  // сначала проверяем, не другой ли день недели уже?
      {
        // начался другой день недели, в который мы можем работать. Для одного дня недели у нас установлена
        // продолжительность полива, поэтому, если мы поливали 28 минут вместо 30, например, во вторник, и перешли на среду,
        // то в среду надо полить 32 мин. Поэтому таймер полива переводим в нужный режим:
        // оставляем в нём недополитое время, чтобы учесть, что поливать надо, например, 32 минуты.
  
        //               разница между полным и отработанным временем
        channel->WateringTimer = -((timeToWatering*60000) - channel->WateringTimer); // загоняем в минус, чтобы добавить недостающие минуты к работе
      }
      
      channel->WateringTimer += dt; // прибавляем время работы
  
      // проверяем, можем ли мы ещё работать
      // если полив уже отработал, и юзер прибавит минуту - мы должны поливать ещё минуту,
      // вне зависимости от показания таймера. Поэтому мы при срабатывании условия окончания полива
      // просто отнимаем дельту времени из таймера, таким образом оставляя его застывшим по времени
      // окончания полива
  
      if(channel->WateringTimer > (timeToWatering*60000) + dt) // приплыли, надо выключать полив
      {
        channel->WateringTimer -= dt;// оставляем таймер застывшим на окончании полива, плюс маленькая дельта
        channel->IsChannelRelayOn = false;
      }
      else
        channel->IsChannelRelayOn = true; // ещё можем работать, продолжаем поливать
     } // else

  
}
void WateringModule::HoldChannelState(int8_t channelIdx, WateringChannel* channel)
{
    uint8_t state = channel->IsChannelRelayOn ? RELAY_ON : RELAY_OFF;

    if(channelIdx == -1) // работаем со всеми каналами
    {
      for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
      {
        digitalWrite(WATER_RELAYS[i],state);
        
         uint8_t idx = i/8; // выясняем, какой индекс
         uint8_t bitNum1 = i % 8;
         OneState* os = State.GetState(StateRelay,idx);
         if(os)
         {
          uint8_t curRelayStates = *((uint8_t*) os->Data); // получаем текущую маску состояния реле
          bitWrite(curRelayStates,bitNum1, channel->IsChannelRelayOn);
          State.UpdateState(StateRelay,idx,(void*)&curRelayStates);
         }
      
      }   
      return;
    } // if

    // работаем с одним каналом
    digitalWrite(WATER_RELAYS[channelIdx],state);

     uint8_t idx = channelIdx/8; // выясняем, какой индекс
     uint8_t bitNum1 = channelIdx % 8;
     OneState* os = State.GetState(StateRelay,idx);
     if(os)
     {
      uint8_t curRelayStates = *((uint8_t*) os->Data); // получаем текущую маску состояния реле
      bitWrite(curRelayStates,bitNum1, channel->IsChannelRelayOn);
      State.UpdateState(StateRelay,idx,(void*)&curRelayStates);
     }
     
    //State.SetRelayState(channelIdx,channel->IsChannelRelayOn);
    
}

bool WateringModule::IsAnyChannelActive(WateringOption wateringOption)
{  
   if(workMode == wwmManual) // в ручном режиме мы управляем только всеми каналами сразу
    return dummyAllChannels.IsChannelRelayOn; // поэтому смотрим состояние реле на всех каналах

    // в автоматическом режиме мы можем рулить как всеми каналами вместе (wateringOption == wateringWeekDays),
    // так и по отдельности (wateringOption == wateringSeparateChannels). В этом случае надо выяснить, состояние каких каналов
    // смотреть, чтобы понять - активен ли кто-то.

    if(wateringOption == wateringWeekDays)
      return dummyAllChannels.IsChannelRelayOn; // смотрим состояние реле на всех каналах

    // тут мы рулим всеми каналами по отдельности, поэтому надо проверить - включено ли реле на каком-нибудь из каналов
    for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
    {
      if(wateringChannels[i].IsChannelRelayOn)
        return true;
    }

    return false;
}
#ifdef USE_PUMP_RELAY
void WateringModule::HoldPumpState(WateringOption wateringOption)
{
  // поддерживаем состояние реле насоса
    bool bPumpIsOn = false;
    
    if(settings->GetTurnOnPump() == 1) // если мы должны включать насос при поливе на любом из каналов
      bPumpIsOn = IsAnyChannelActive(wateringOption); // то делаем это только тогда, когда полив включен на любом из каналов

    // пишем в реле насоса вкл или выкл в зависимости от настройки "включать насос при поливе"
    uint8_t state = bPumpIsOn ? RELAY_ON : RELAY_OFF;
    digitalWrite(PUMP_RELAY_PIN,state); 
}
#endif

void WateringModule::Update(uint16_t dt)
{ 
   WateringOption wateringOption = settings->GetWateringOption(); // получаем опцию управления поливом

#ifdef USE_PUMP_RELAY
  // держим состояние реле для насоса
  HoldPumpState(wateringOption);
#endif

  #ifdef USE_DS3231_REALTIME_CLOCK

    // обновляем состояние часов
    DS3231Clock watch =  controller->GetClock();
    DS3231Time t =   watch.getTime();
    
    if(currentDOW == -1) // если мы не сохраняли текущий день недели, то
    {
      currentDOW = t.dayOfWeek; // сохраним его, чтобы потом проверять переход через дни недели
      lastDOW = t.dayOfWeek; // сохраним и как предыдущий день недели
    }

    if(currentDOW != t.dayOfWeek)
    {
      // начался новый день недели, принудительно переходим в автоматический режим работы
      // даже если до этого был включен полив командой от пользователя
      workMode = wwmAutomatic;
    }

    currentDOW = t.dayOfWeek; // сохраняем текущий день недели
    currentHour = t.hour; // сохраняем текущий час
       
  #else

    // модуль часов реального времени не включен в компиляцию, деградируем до ручного режима работы
    settings->SetWateringOption(wateringOFF); // отключим автоматический контроль полива
    workMode = wwmManual; // переходим на ручное управление
    blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
 
  #endif
  
  if(workMode == wwmAutomatic)
  {
    // автоматический режим работы
 
    // проверяем текущий режим управления каналами полива
    switch(wateringOption)
    {
      case wateringOFF: // автоматическое управление поливом выключено, значит, мы должны перейти в ручной режим работы
          workMode = wwmManual; // переходим в ручной режим работы
          blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
      break;

      case wateringWeekDays: // // управление поливом по дням недели (все каналы одновременно)
      {
          // обновляем состояние всех каналов - канал станет активным или неактивным после этой проверки
           UpdateChannel(-1,&dummyAllChannels,dt);
           
           // теперь держим текущее состояние реле на всех каналах
           HoldChannelState(-1,&dummyAllChannels);
      }
      break;

      case wateringSeparateChannels: // рулим всеми каналами по отдельности
      {
        for(uint8_t i=0;i<WATER_RELAYS_COUNT;i++)
        {
          UpdateChannel(i,&(wateringChannels[i]),dt); // обновляем канал
          HoldChannelState(i,&(wateringChannels[i]));  // держим его состояние
        } // for
      }
      break;
      
    } // switch(wateringOption)
  }
  else
  {
    // ручной режим работы, просто сохраняем переданный нам статус реле, все каналы - одновременно.
    // обновлять состояние канала не надо, потому что мы в ручном режиме работы.
      HoldChannelState(-1,&dummyAllChannels);
          
  } // else

  // обновили все каналы, теперь можно сбросить флаг перехода через день недели
  lastDOW = currentDOW; // сделаем вид, что мы ничего не знаем о переходе на новый день недели.
  // таким образом, код перехода на новый день недели выполнится всего один раз при каждом переходе
  // через день недели.
  
}
bool  WateringModule::ExecCommand(const Command& command)
{
  ModuleController* c = GetController();
  GlobalSettings* settings = c->GetSettings();
  
  String answer; answer.reserve(RESERVE_STR_LENGTH);
  answer = UNKNOWN_COMMAND;
  
  bool answerStatus = false; 
  
  if(command.GetType() == ctSET) 
  {
      uint8_t argsCount = command.GetArgsCount();
      
      if(argsCount < 1) // не хватает параметров
      {
        answerStatus = false;
        answer = PARAMS_MISSED;
      }
      else
      {
        String which = command.GetArg(0);
        which.toUpperCase();

        if(which == WATER_SETTINGS_COMMAND)
        {
          if(argsCount > 5)
          {
              // парсим параметры
              WateringOption wateringOption = (WateringOption) command.GetArg(1).toInt();
              uint8_t wateringWeekDays = command.GetArg(2).toInt();
              uint16_t wateringTime = command.GetArg(3).toInt();
              uint8_t startWateringTime = command.GetArg(4).toInt();
              uint8_t turnOnPump = command.GetArg(5).toInt();
      
              // пишем в настройки
              settings->SetWateringOption(wateringOption);
              settings->SetWateringWeekDays(wateringWeekDays);
              settings->SetWateringTime(wateringTime);
              settings->SetStartWateringTime(startWateringTime);
              settings->SetTurnOnPump(turnOnPump);
      
              // сохраняем настройки
              settings->Save();

              if(wateringOption == wateringOFF) // если выключено автоуправление поливом
              {
                workMode = wwmManual; // переходим в ручной режим работы
                dummyAllChannels.IsChannelRelayOn = false; // принудительно гасим полив на всех каналах
                blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
              }
              else
              {
                workMode = wwmAutomatic; // иначе переходим в автоматический режим работы
                blinker.blink(); // гасим диод
              }
      
              
              answerStatus = true;
              answer = WATER_SETTINGS_COMMAND; answer += PARAM_DELIMITER;
              answer += REG_SUCC;
          } // argsCount > 3
          else
          {
            // не хватает команд
            answerStatus = false;
            answer = PARAMS_MISSED;
          }
          
        } // WATER_SETTINGS_COMMAND
        else
        if(which == WATER_CHANNEL_SETTINGS) // настройки канала CTSET=WATER|CH_SETT|IDX|WateringDays|WateringTime|StartTime
        {
           if(argsCount > 4)
           {
                uint8_t channelIdx = command.GetArg(1).toInt();
                if(channelIdx < WATER_RELAYS_COUNT)
                {
                  // нормальный индекс
                  uint8_t wDays = command.GetArg(2).toInt();
                  uint16_t wTime = command.GetArg(3).toInt();
                  uint8_t sTime = command.GetArg(4).toInt();
                  
                  settings->SetChannelWateringWeekDays(channelIdx,wDays);
                  settings->SetChannelWateringTime(channelIdx,wTime);
                  settings->SetChannelStartWateringTime(channelIdx,sTime);
                  
                  answerStatus = true;
                  answer = WATER_CHANNEL_SETTINGS; answer += PARAM_DELIMITER;
                  answer += command.GetArg(1); answer += PARAM_DELIMITER;
                  answer += REG_SUCC;
                 
                }
                else
                {
                  // плохой индекс
                  answerStatus = false;
                  answer = UNKNOWN_COMMAND;
                }
           }
           else
           {
            // не хватает команд
            answerStatus = false;
            answer = PARAMS_MISSED;            
           }
        }
        else
        if(which == WORK_MODE) // CTSET=WATER|MODE|AUTO, CTSET=WATER|MODE|MANUAL
        {
           // попросили установить режим работы
           String param = command.GetArg(1);
           
           if(param == WM_AUTOMATIC)
           {
             workMode = wwmAutomatic; // переходим в автоматический режим работы
             blinker.blink(); // гасим диод
           }
           else
           {
            workMode = wwmManual; // переходим на ручной режим работы
            blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
           }

              answerStatus = true;
              answer = WORK_MODE; answer += PARAM_DELIMITER;
              answer += param;
        
        } // WORK_MODE
        else 
        if(which == STATE_ON) // попросили включить полив, CTSET=WATER|ON
        {
          if(!command.IsInternal()) // если команда от юзера, то
          {
            workMode = wwmManual; // переходим в ручной режим работы
            blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
          }
          // если команда не от юзера, а от модуля ALERT, например, то
          // просто выставляя статус реле для всех каналов - мы ничего не добьёмся - 
          // команда проигнорируется, т.к. мы сами обновляем статус каналов.
          // в этом случае - надо переходить на ручное управление, мне кажется.
          // Хотя это - неправильно, должна быть возможность в автоматическом
          // режиме включать/выключать полив из модуля ALERT, без мигания диодом.

          dummyAllChannels.IsChannelRelayOn = true; // включаем реле на всех каналах

          answerStatus = true;
          answer = STATE_ON;
        } // STATE_ON
        else 
        if(which == STATE_OFF) // попросили выключить полив, CTSET=WATER|OFF
        {
          if(!command.IsInternal()) // если команда от юзера, то
          {
            workMode = wwmManual; // переходим в ручной режим работы
            blinker.blink(WORK_MODE_BLINK_INTERVAL); // зажигаем диод
          }
          // если команда не от юзера, а от модуля ALERT, например, то
          // просто выставляя статус реле для всех каналов - мы ничего не добьёмся - 
          // команда проигнорируется, т.к. мы сами обновляем статус каналов.
          // в этом случае - надо переходить на ручное управление, мне кажется.
          // Хотя это - неправильно, должна быть возможность в автоматическом
          // режиме включать/выключать полив из модуля ALERT, без мигания диодом.

          dummyAllChannels.IsChannelRelayOn = false; // выключаем реле на всех каналах

          answerStatus = true;
          answer = STATE_OFF;
          
        } // STATE_OFF        

      } // else
  }
  else
  if(command.GetType() == ctGET) //получить данные
  {

    String t = command.GetRawArguments();
    t.toUpperCase();
    
    if(t == GetID()) // нет аргументов, попросили вернуть статус полива
    {
      answerStatus = true;
      answer = IsAnyChannelActive(settings->GetWateringOption()) ? STATE_ON : STATE_OFF;
      answer += PARAM_DELIMITER;
      answer += workMode == wwmAutomatic ? WM_AUTOMATIC : WM_MANUAL;
    }
    else
    if(t == WATER_SETTINGS_COMMAND) // запросили данные о настройках полива
    {
      answerStatus = true;
      answer = WATER_SETTINGS_COMMAND; answer += PARAM_DELIMITER; 
      answer += String(settings->GetWateringOption()); answer += PARAM_DELIMITER;
      answer += String(settings->GetWateringWeekDays()); answer += PARAM_DELIMITER;
      answer += String(settings->GetWateringTime()); answer += PARAM_DELIMITER;
      answer += String(settings->GetStartWateringTime()); answer += PARAM_DELIMITER;
      answer += String(settings->GetTurnOnPump());
    }
    else
    if(t == WATER_CHANNELS_COUNT_COMMAND)
    {
      answerStatus = true;
      answer = WATER_CHANNELS_COUNT_COMMAND; answer += PARAM_DELIMITER;
      answer += String(WATER_RELAYS_COUNT);
      
    }
    else
    if(t == WORK_MODE) // получить режим работы
    {
      answerStatus = true;
      answer = WORK_MODE; answer += PARAM_DELIMITER;
      answer += workMode == wwmAutomatic ? WM_AUTOMATIC : WM_MANUAL;
    }
    else
    {
       // команда с аргументами
       uint8_t argsCnt = command.GetArgsCount();
       if(argsCnt > 1)
       {
            t = command.GetArg(0);
            t.toUpperCase();

            if(t == WATER_CHANNEL_SETTINGS)
            {
              // запросили настройки канала
              uint8_t idx = command.GetArg(1).toInt();
              
              if(idx < WATER_RELAYS_COUNT)
              {
                answerStatus = true;
             
                answer = WATER_CHANNEL_SETTINGS; answer += PARAM_DELIMITER;
                answer += command.GetArg(1); answer += PARAM_DELIMITER; 
                answer += String(settings->GetChannelWateringWeekDays(idx)); answer += PARAM_DELIMITER;
                answer += String(settings->GetChannelWateringTime(idx)); answer += PARAM_DELIMITER;
                answer += String(settings->GetChannelStartWateringTime(idx));
              }
              else
              {
                // плохой индекс
                answerStatus = false;
                answer = UNKNOWN_COMMAND;
              }
                      
            } // if
       } // if
    } // else
    
  } // if
 
 // отвечаем на команду
    SetPublishData(&command,answerStatus,answer); // готовим данные для публикации
    c->Publish(this);
    
  return answerStatus;
}
