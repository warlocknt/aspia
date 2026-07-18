Aspia
=====

> [!WARNING]
> **Версия нестабильна. Ведётся работа по стабилизации. Для промышленной эксплуатации не рекомендуется — только тестирование.**
>
> В период стабилизации используйте все компоненты из одной сборки: правки пока вносятся без оглядки на совместимость версий. Там, где совместимость окажется возможной, она будет сохранена.
>
> Пока идёт стабилизация, собираются и проверяются только сборки для Windows. Поддержка Linux и macOS в исходном коде сохранена, но здесь не собирается и не тестируется.
>
> Это форк [dchapyshev/aspia](https://github.com/dchapyshev/aspia). Ветка `stabilize/3.0.0-qt5` основана на коммите `fcec8cf5` оригинального репозитория — это последнее состояние версии 3.0.0 на Qt5, непосредственно перед началом переноса проекта на Qt6 (следующий коммит upstream — `c24346c3` «Porting to qt6»).
>
> Версия 3.0.0 автором официально не выпускалась: последний релиз upstream — [v2.7.0](https://github.com/dchapyshev/aspia/releases) (май 2024). Здесь она дорабатывается: исправляются ошибки, выявленные при реальной эксплуатации.
>
> **Для рабочих задач используйте официальный релиз v2.7.0.**

> [!WARNING]
> **This version is unstable. Stabilization is in progress. Not recommended for production use — testing only.**
>
> While stabilization is under way, use all components from the same build: changes are currently made without regard for version compatibility. Where compatibility turns out to be feasible, it will be preserved.
>
> For now only Windows builds are produced and tested. Linux and macOS support is kept in the source, but is neither built nor tested here.
>
> This is a fork of [dchapyshev/aspia](https://github.com/dchapyshev/aspia). The `stabilize/3.0.0-qt5` branch is based on commit `fcec8cf5` of the original repository — the last state of version 3.0.0 on Qt5, immediately before the project's migration to Qt6 began (the next upstream commit is `c24346c3` "Porting to qt6").
>
> Version 3.0.0 has never been officially released by the author: the latest upstream release is [v2.7.0](https://github.com/dchapyshev/aspia/releases) (May 2024). This fork works on it: fixing issues found in real-world use.
>
> **For production, please use the official v2.7.0 release.**

Что нового в этой ветке
-----------------------

Изменения относительно исходного коммита `fcec8cf5`.

### Исправления

**Захват экрана зависал после разблокировки компьютера.**
При подключении к заблокированному компьютеру после ввода пароля картинка замирала: приходил один и тот же частично отрисованный кадр, при этом соединение считалось исправным и не разрывалось. Переподключение не помогало.
Причина: интерфейс дублирования рабочего стола (DXGI Desktop Duplication) привязан к рабочему столу, на котором был создан. После перехода с экрана блокировки на рабочий стол пользователя он переставал отдавать кадры, но не сообщал об ошибке `DXGI_ERROR_ACCESS_LOST` — вместо этого бесконечно возвращал `DXGI_ERROR_WAIT_TIMEOUT`, что трактовалось как «на экране ничего не изменилось». Дублирование при этом никогда не пересоздавалось, а клиенту повторно отправлялся устаревший кадр, сохранённый на момент переключения.
Исправление: при смене рабочего стола объекты дублирования освобождаются, и захват заново привязывается к текущему рабочему столу — так же, как это уже делал GDI-захватчик.
*Статус: исправление собрано, но на проблемном компьютере ещё не проверено.*

**Задержки в работе и «слайд-шоу» при активности на экране.**
Работать было тяжело: изображение обновлялось рывками, ввод отставал, при подключении двух консолей становилось заметно хуже.
Причина: масштабирование и кодирование видео выполнялись синхронно в главном потоке, вместе с сетевым вводом-выводом, межпроцессным обменом и передачей ввода. Один поток оказывался загружен полностью, а остальные ядра простаивали; при нескольких клиентах кодирование для них шло последовательно.
Исправление: кодирование вынесено в отдельный поток для каждого клиента. Кадр копируется из разделяемой памяти до передачи в поток, поэтому буфер захвата можно переиспользовать сразу. Если кодировщик ещё занят, кадр пропускается, а не ставится в очередь — для видео в реальном времени свежий кадр важнее полного.
*Статус: подтверждено измерением — нагрузка распределилась по потокам, ни одно ядро не загружено полностью, потребление памяти стабильно.*

**Некорректные зашифрованные пакеты могли завершить процесс аварийно.**
Сообщение короче тега аутентификации приводило к вычислению отрицательной длины и чтению за границей буфера. Проверка добавлена в расшифровщик (перенесено из upstream) и дополнительно у вызывающей стороны: в нашей ветке размеры беззнаковые, поэтому вычитание заворачивалось в огромное значение, и выделение буфера происходило до проверки.

**Передача файлов на заблокированный компьютер сообщала о неверной причине.**
При попытке передать файлы на компьютер с заблокированным экраном операция отклонялась с сообщением «Нет пользователя, вошедшего в систему» — хотя пользователь в системе был, просто экран заблокирован. Причина отказа выглядела необъяснимой, и такое поведение обычно принимали за должное.
Исправление: для заблокированной сессии добавлен отдельный код ошибки, и сообщение теперь подсказывает, что делать — разблокировать компьютер. Сам запрет на передачу при блокировке сохранён намеренно: это защита от выгрузки файлов с оставленного без присмотра рабочего места. Теперь по сообщению видно, требуется разблокировать компьютер или войти в систему.

**Передача файлов могла прекратиться без сообщений.**
Если от хоста приходил ответ, который не удавалось разобрать, обработчик завершался, не сняв запрос с очереди и ничего не сообщив. Ответы сопоставляются с запросами по порядку очереди, а следующий запрос отправляется только при получении ответа, — поэтому очередь больше нечем было разгрести, и все последующие операции молча в ней накапливались.
Исправление: запрос снимается с очереди, а вместо тишины показывается сообщение об ошибке.

### Обновление и диагностика

**Установка новой сборки поверх предыдущей.**
Пакеты MSI не обновлялись поверх уже установленных — требовалось сначала удалить программу. Причина: сборки выпускались с одинаковой версией, а обновление равной версии было запрещено (`AllowSameVersionUpgrades="no"`), при этом код продукта генерировался заново, и установщик считал пакет другим продуктом. Теперь обновление равной версии разрешено во всех пакетах.

**Определение сборки по журналу.**
Раньше по журналу нельзя было понять, какая именно сборка работает: номер версии включает число коммитов, поэтому разные сборки одного коммита неразличимы. Теперь при запуске записываются:
- время компоновки модуля (берётся из заголовка исполняемого файла, уникально для каждой сборки);
- отметка, если сборка сделана из изменённых, но не зафиксированных исходников;
- перечень загруженных модулей Aspia с версией и временем компоновки каждого.
Если версии модулей не совпадают (например, исполняемый файл от одной сборки, библиотека от другой — обычное следствие неудачного обновления), в журнал записывается предупреждение. Работа при этом не прерывается: версии могут оставаться совместимыми.

**Стартовые сведения записываются всегда**, независимо от настроенного уровня подробности. Иначе на компьютерах, где ведётся запись только ошибок, журнал оказывался пустым — именно там, откуда его обычно и запрашивают. Записывается один раз за запуск, на размер журнала не влияет.

### Сборка

- Номер версии берётся из `CMakeLists.txt` — одним источником и для исполняемых файлов, и для пакетов MSI.
- Путь к WDK можно задать явно через `WDK_TARGET_VERSION`, вместо поиска произвольной установленной версии.
- Исправлены пути включаемых файлов для генерируемого кода protobuf.
- В инсталляторе исправлено имя файла `aspia_file_agent.exe` (был переименован в исходном коде).

Remote desktop, file transfer and system information tool.

With Aspia, you can create your own NAT traversal infrastructure (using Router and Relay servers) with connection by ID or use direct connections. Aspia supports many features. Among them, detailed information about the system, audio, text chat.

|Build Status|
|:--:|
|[![Build status](https://github.com/dchapyshev/aspia/actions/workflows/windows.yml/badge.svg?branch=master)](https://github.com/dchapyshev/aspia/actions/workflows/windows.yml) [![Build status](https://github.com/dchapyshev/aspia/actions/workflows/linux.yml/badge.svg?branch=master)](https://github.com/dchapyshev/aspia/actions/workflows/linux.yml) [![Build status](https://github.com/dchapyshev/aspia/actions/workflows/macos.yml/badge.svg?branch=master)](https://github.com/dchapyshev/aspia/actions/workflows/macos.yml)|

Currently supported
-------------------
- Remote desktop management
- Remote desktop view
- File transfer
- System information
- Text chat
- Task manager
- Encryption
- Authorization (it is possible to add users with different access rights)
- Address book with encryption and master-password
- <b>NAT traversal with connection by ID</b> (with using Aspia Router and Aspia Relay)
- Direct connections
- Audio support
- Video recording
- Client and Console for Windows, MacOSX and Linux
- Host for Windows only
- Router/Relay for Windows and Linux
- And much more

System requirements
-------------------
- Windows 7/2008 R2 or higher (x86 or x86_64 CPU)
- Debian 11/Ubuntu 20.04 Linux (x86_64 CPU)
- MacOSX (x86_64 or ARM64 CPU)

Contacts
--------
E-Mail: dmitry@aspia.ru

Group in Telegram: [@aspia_talks](https://t.me/aspia_talks)

News in Telegram: [@aspia_news](https://t.me/aspia_news)

Licensing
---------
Project code is available under the GNU General Public License 3.

For more information, see [license agreement](LICENSE.md).

See also
--------
- [Documentation](https://aspia.org/documentation.html)
- [Instructions for building the project (Windows)](https://aspia.org/docs/building-windows)
- [Instructions for building the project (Linux)](https://aspia.org/docs/building-linux)
- [Instructions for building the project (MacOSX)](https://aspia.org/docs/building-macos)
- [Instructions for translators](https://aspia.org/docs/translators)
- [Code of conduct](CODE_OF_CONDUCT.md)
