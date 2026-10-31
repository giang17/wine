/*
 * Copyright 2026 Giang Nguyen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#define COBJMACROS
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winstring.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Globalization
#include "windows.globalization.h"

#include "wine/test.h"


struct hstring_iterable
{
    IIterable_HSTRING IIterable_HSTRING_iface;
    IIterator_HSTRING IIterator_HSTRING_iface;
    LONG ref;
    UINT32 count, index;
    HSTRING values[8];
};

static struct hstring_iterable *impl_from_IIterable_HSTRING( IIterable_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterable, IIterable_HSTRING_iface );
}

static struct hstring_iterable *impl_from_IIterator_HSTRING( IIterator_HSTRING *iface )
{
    return CONTAINING_RECORD( iface, struct hstring_iterable, IIterator_HSTRING_iface );
}

static HRESULT WINAPI iterable_QueryInterface( IIterable_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterable_HSTRING ))
    {
        IIterable_HSTRING_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterable_AddRef( IIterable_HSTRING *iface )
{
    return InterlockedIncrement( &impl_from_IIterable_HSTRING( iface )->ref );
}

static ULONG WINAPI iterable_Release( IIterable_HSTRING *iface )
{
    struct hstring_iterable *impl = impl_from_IIterable_HSTRING( iface );
    ULONG ref = InterlockedDecrement( &impl->ref ), i;
    if (!ref)
    {
        for (i = 0; i < impl->count; i++) WindowsDeleteString( impl->values[i] );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI iterable_GetIids( IIterable_HSTRING *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI iterable_GetRuntimeClassName( IIterable_HSTRING *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI iterable_GetTrustLevel( IIterable_HSTRING *iface, TrustLevel *level ) { return E_NOTIMPL; }

static HRESULT WINAPI iterable_First( IIterable_HSTRING *iface, IIterator_HSTRING **value )
{
    struct hstring_iterable *impl = impl_from_IIterable_HSTRING( iface );
    impl->index = 0;
    IIterable_HSTRING_AddRef( iface );
    *value = &impl->IIterator_HSTRING_iface;
    return S_OK;
}

static const IIterable_HSTRINGVtbl iterable_vtbl =
{
    iterable_QueryInterface, iterable_AddRef, iterable_Release,
    iterable_GetIids, iterable_GetRuntimeClassName, iterable_GetTrustLevel,
    iterable_First,
};

static HRESULT WINAPI iterator_QueryInterface( IIterator_HSTRING *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_HSTRING ))
    {
        IIterator_HSTRING_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI iterator_AddRef( IIterator_HSTRING *iface )
{
    return iterable_AddRef( &impl_from_IIterator_HSTRING( iface )->IIterable_HSTRING_iface );
}

static ULONG WINAPI iterator_Release( IIterator_HSTRING *iface )
{
    return iterable_Release( &impl_from_IIterator_HSTRING( iface )->IIterable_HSTRING_iface );
}

static HRESULT WINAPI iterator_GetIids( IIterator_HSTRING *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI iterator_GetRuntimeClassName( IIterator_HSTRING *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI iterator_GetTrustLevel( IIterator_HSTRING *iface, TrustLevel *level ) { return E_NOTIMPL; }

static HRESULT WINAPI iterator_get_Current( IIterator_HSTRING *iface, HSTRING *value )
{
    struct hstring_iterable *impl = impl_from_IIterator_HSTRING( iface );
    if (impl->index >= impl->count) return E_BOUNDS;
    return WindowsDuplicateString( impl->values[impl->index], value );
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterable *impl = impl_from_IIterator_HSTRING( iface );
    *value = impl->index < impl->count;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_HSTRING *iface, boolean *value )
{
    struct hstring_iterable *impl = impl_from_IIterator_HSTRING( iface );
    if (impl->index < impl->count) impl->index++;
    *value = impl->index < impl->count;
    return S_OK;
}

static HRESULT WINAPI iterator_GetMany( IIterator_HSTRING *iface, UINT32 size, HSTRING *items, UINT32 *count )
{
    struct hstring_iterable *impl = impl_from_IIterator_HSTRING( iface );
    UINT32 i;
    for (i = 0; i < size && impl->index + i < impl->count; i++)
        WindowsDuplicateString( impl->values[impl->index + i], &items[i] );
    *count = i;
    return S_OK;
}

static const IIterator_HSTRINGVtbl iterator_vtbl =
{
    iterator_QueryInterface, iterator_AddRef, iterator_Release,
    iterator_GetIids, iterator_GetRuntimeClassName, iterator_GetTrustLevel,
    iterator_get_Current, iterator_get_HasCurrent, iterator_MoveNext, iterator_GetMany,
};

static IIterable_HSTRING *create_languages( const WCHAR **langs, UINT32 count )
{
    struct hstring_iterable *impl = calloc( 1, sizeof(*impl) );
    UINT32 i;

    impl->IIterable_HSTRING_iface.lpVtbl = &iterable_vtbl;
    impl->IIterator_HSTRING_iface.lpVtbl = &iterator_vtbl;
    impl->ref = 1;
    impl->count = count;
    for (i = 0; i < count; i++) WindowsCreateString( langs[i], wcslen( langs[i] ), &impl->values[i] );
    return &impl->IIterable_HSTRING_iface;
}

static HSTRING hs( const WCHAR *str )
{
    HSTRING ret = NULL;
    if (str) WindowsCreateString( str, wcslen( str ), &ret );
    return ret;
}


static const char *dbg_hs( HSTRING str )
{
    return debugstr_w( WindowsGetStringRawBuffer( str, NULL ) );
}

static INT64 make_datetime( int year, int month, int day, int hour, int minute, int second )
{
    SYSTEMTIME st = {year, month, 0, day, hour, minute, second, 0};
    FILETIME ft;
    SystemTimeToFileTime( &st, &ft );
    return ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static IActivationFactory *get_factory(void)
{
    IActivationFactory *factory = NULL;
    HSTRING str = hs( L"Windows.Globalization.Calendar" );
    HRESULT hr;

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "RoGetActivationFactory failed, hr %#lx\n", hr );
    return factory;
}

/* NULL for system, clock or time_zone leaves the argument out; lang NULL passes no language list */
static HRESULT create_calendar( const WCHAR *lang, const WCHAR *system, const WCHAR *clock, const WCHAR *time_zone,
                                ICalendar **calendar )
{
    IActivationFactory *factory = get_factory();
    IIterable_HSTRING *languages = NULL;
    ICalendarFactory2 *factory2;
    ICalendarFactory *factory1;
    HSTRING hsystem = hs( system ), hclock = hs( clock ), htz = hs( time_zone );
    HRESULT hr;

    *calendar = NULL;
    if (lang) languages = create_languages( &lang, 1 );
    if (time_zone)
    {
        hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory2, (void **)&factory2 );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = ICalendarFactory2_CreateCalendarWithTimeZone( factory2, languages, hsystem, hclock, htz, calendar );
        ICalendarFactory2_Release( factory2 );
    }
    else
    {
        hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory, (void **)&factory1 );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        if (system) hr = ICalendarFactory_CreateCalendar( factory1, languages, hsystem, hclock, calendar );
        else hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, languages, calendar );
        ICalendarFactory_Release( factory1 );
    }
    if (languages) IIterable_HSTRING_Release( languages );
    WindowsDeleteString( hsystem );
    WindowsDeleteString( hclock );
    WindowsDeleteString( htz );
    IActivationFactory_Release( factory );
    return hr;
}

static ICalendar *create_utc_calendar( const WCHAR *clock )
{
    ICalendar *calendar;
    HRESULT hr = create_calendar( L"en-US", L"GregorianCalendar", clock, L"UTC", &calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    return calendar;
}

static void set_datetime( ICalendar *calendar, INT64 value )
{
    DateTime dt = {value};
    HRESULT hr = ICalendar_SetDateTime( calendar, dt );
    ok( hr == S_OK, "SetDateTime failed, hr %#lx\n", hr );
}

static INT64 get_datetime( ICalendar *calendar )
{
    DateTime dt = {0};
    HRESULT hr = ICalendar_GetDateTime( calendar, &dt );
    ok( hr == S_OK, "GetDateTime failed, hr %#lx\n", hr );
    return dt.UniversalTime;
}

#define check_int( calendar, name, expect ) \
        check_int_( __LINE__, calendar, #name, (calendar)->lpVtbl->get_##name, expect )
static void check_int_( unsigned int line, ICalendar *calendar, const char *name,
                        HRESULT (WINAPI *get)( ICalendar *, INT32 * ), INT32 expect )
{
    INT32 value = -12345;
    HRESULT hr = get( calendar, &value );
    ok_(__FILE__, line)( hr == S_OK, "%s: got hr %#lx.\n", name, hr );
    ok_(__FILE__, line)( value == expect, "%s: got %d, expected %d.\n", name, value, expect );
}

#define check_hstring( str, expect ) check_hstring_( __LINE__, str, expect )
static void check_hstring_( unsigned int line, HSTRING str, const WCHAR *expect )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( str, NULL );
    ok_(__FILE__, line)( !wcscmp( buffer, expect ), "got %s, expected %s.\n", debugstr_w( buffer ), debugstr_w( expect ) );
    WindowsDeleteString( str );
}

#define check_str( calendar, name, expect ) \
        check_str_( __LINE__, calendar, #name, (calendar)->lpVtbl->name, expect )
static void check_str_( unsigned int line, ICalendar *calendar, const char *name,
                        HRESULT (WINAPI *get)( ICalendar *, HSTRING * ), const WCHAR *expect )
{
    HSTRING str = NULL;
    HRESULT hr = get( calendar, &str );
    ok_(__FILE__, line)( hr == S_OK, "%s: got hr %#lx.\n", name, hr );
    check_hstring_( line, str, expect );
}

#define check_str_arg( calendar, name, arg, expect ) \
        check_str_arg_( __LINE__, calendar, #name, (calendar)->lpVtbl->name, arg, expect )
static void check_str_arg_( unsigned int line, ICalendar *calendar, const char *name,
                            HRESULT (WINAPI *get)( ICalendar *, INT32, HSTRING * ), INT32 arg, const WCHAR *expect )
{
    HSTRING str = NULL;
    HRESULT hr = get( calendar, arg, &str );

    if (!expect)
    {
        ok_(__FILE__, line)( hr == E_INVALIDARG, "%s(%d): got hr %#lx.\n", name, arg, hr );
        return;
    }
    ok_(__FILE__, line)( hr == S_OK, "%s(%d): got hr %#lx.\n", name, arg, hr );
    check_hstring_( line, str, expect );
}

static void test_Calendar_activation(void)
{
    IActivationFactory *factory = get_factory();
    ICalendarFactory2 *factory2;
    ICalendarFactory *factory1;
    IVectorView_HSTRING *languages;
    ITimeZoneOnCalendar *time_zone;
    IInspectable *inspectable;
    ICalendar *calendar;
    IAgileObject *agile;
    TrustLevel level;
    FILETIME before;
    INT64 time;
    UINT32 size;
    HSTRING str;
    HRESULT hr;

    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory, (void **)&factory1 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ICalendarFactory_Release( factory1 );
    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory2, (void **)&factory2 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ICalendarFactory2_Release( factory2 );
    hr = IActivationFactory_GetRuntimeClassName( factory, &str );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IActivationFactory_GetTrustLevel( factory, &level );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( level == BaseTrust, "got level %d.\n", level );

    GetSystemTimeAsFileTime( &before );
    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IActivationFactory_Release( factory );

    hr = IInspectable_QueryInterface( inspectable, &IID_ICalendar, (void **)&calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_ITimeZoneOnCalendar, (void **)&time_zone );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_IAgileObject, (void **)&agile );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IAgileObject_Release( agile );
    IInspectable_Release( inspectable );

    hr = ICalendar_GetRuntimeClassName( calendar, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_hstring( str, L"Windows.Globalization.Calendar" );
    hr = ICalendar_GetTrustLevel( calendar, &level );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( level == BaseTrust, "got level %d.\n", level );

    /* a new calendar holds the current time, in the Gregorian calendar */
    time = get_datetime( calendar ) - (((INT64)before.dwHighDateTime << 32) | before.dwLowDateTime);
    ok( time >= -1000000 && time < 100000000, "got time offset %I64d.\n", time );
    check_str( calendar, GetCalendarSystem, L"GregorianCalendar" );
    check_int( calendar, Era, 1 );

    hr = ICalendar_get_Languages( calendar, &languages );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVectorView_HSTRING_get_Size( languages, &size );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( size >= 1, "got size %u.\n", size );
    IVectorView_HSTRING_Release( languages );

    hr = ITimeZoneOnCalendar_GetTimeZone( time_zone, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !WindowsIsStringEmpty( str ), "got empty time zone.\n" );
    WindowsDeleteString( str );

    ITimeZoneOnCalendar_Release( time_zone );
    ICalendar_Release( calendar );
}

static void test_Calendar_factory(void)
{
    static const struct
    {
        const WCHAR *lang, *system, *clock, *time_zone;
        HRESULT hr;
        const WCHAR *resolved_zone;
        BOOL todo;
    }
    tests[] =
    {
        {L"en-US", NULL, NULL, NULL, S_OK},
        {L"", NULL, NULL, NULL, E_INVALIDARG},
        {L"xx-invalid", NULL, NULL, NULL, S_OK},
        {L"en-US", L"GregorianCalendar", L"12HourClock", NULL, S_OK},
        {L"en-US", L"gregoriancalendar", L"12HourClock", NULL, E_INVALIDARG},
        {L"en-US", L"Foo", L"12HourClock", NULL, E_INVALIDARG},
        {L"en-US", L"JapaneseCalendar", L"12HourClock", NULL, S_OK, NULL, TRUE},
        {L"en-US", L"GregorianCalendar", L"24hourclock", NULL, E_INVALIDARG},
        {L"en-US", L"GregorianCalendar", L"", NULL, E_INVALIDARG},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"UTC", S_OK, L"Etc/UTC"},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"GMT", S_OK, L"Etc/GMT"},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"europe/berlin", S_OK, L"Europe/Berlin"},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"Asia/Kolkata", S_OK, L"Asia/Calcutta"},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"W. Europe Standard Time", E_INVALIDARG},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"Invalid/Zone", E_INVALIDARG},
        {L"en-US", L"GregorianCalendar", L"24HourClock", L"", E_INVALIDARG},
    };
    IActivationFactory *factory = get_factory();
    ITimeZoneOnCalendar *time_zone;
    IIterable_HSTRING *languages;
    IVectorView_HSTRING *view;
    ICalendarFactory *factory1;
    ICalendar *calendar;
    unsigned int i;
    UINT32 size;
    HSTRING str;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        winetest_push_context( "%u", i );
        hr = create_calendar( tests[i].lang, tests[i].system, tests[i].clock, tests[i].time_zone, &calendar );
        todo_wine_if( tests[i].todo )
        ok( hr == tests[i].hr, "got hr %#lx.\n", hr );
        if (calendar && tests[i].resolved_zone)
        {
            hr = ICalendar_QueryInterface( calendar, &IID_ITimeZoneOnCalendar, (void **)&time_zone );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            hr = ITimeZoneOnCalendar_GetTimeZone( time_zone, &str );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            check_hstring( str, tests[i].resolved_zone );
            ITimeZoneOnCalendar_Release( time_zone );
        }
        if (calendar && tests[i].clock) check_str( calendar, GetClock, tests[i].clock );
        if (calendar) ICalendar_Release( calendar );
        winetest_pop_context();
    }

    /* the language list is kept as given, invalid tags included */
    hr = create_calendar( L"xx-invalid", NULL, NULL, NULL, &calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = ICalendar_get_Languages( calendar, &view );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IVectorView_HSTRING_get_Size( view, &size );
    ok( hr == S_OK && size == 1, "got hr %#lx, size %u.\n", hr, size );
    hr = IVectorView_HSTRING_GetAt( view, 0, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_hstring( str, L"xx-invalid" );
    IVectorView_HSTRING_Release( view );
    ICalendar_Release( calendar );

    hr = create_calendar( L"en-US", NULL, NULL, NULL, &calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_str( calendar, get_ResolvedLanguage, L"en-US" );
    check_str( calendar, GetClock, L"12HourClock" );
    check_str( calendar, get_NumeralSystem, L"Latn" );
    ICalendar_Release( calendar );

    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory, (void **)&factory1 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    calendar = (ICalendar *)0xdeadbeef;
    hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, NULL, &calendar );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    languages = create_languages( NULL, 0 );
    hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, languages, &calendar );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    IIterable_HSTRING_Release( languages );
    ICalendarFactory_Release( factory1 );
    IActivationFactory_Release( factory );
}

static void test_Calendar_fields(void)
{
    ICalendar *calendar = create_utc_calendar( L"24HourClock" );
    DayOfWeek day_of_week;
    boolean dst;
    HRESULT hr;

    /* 2024-02-29T13:45:30.1234567Z, a Thursday */
    set_datetime( calendar, make_datetime( 2024, 2, 29, 13, 45, 30 ) + 1234567 );

    check_int( calendar, Era, 1 );
    check_int( calendar, FirstEra, 1 );
    check_int( calendar, LastEra, 1 );
    check_int( calendar, NumberOfEras, 1 );
    check_int( calendar, Year, 2024 );
    check_int( calendar, FirstYearInThisEra, 1 );
    check_int( calendar, LastYearInThisEra, 9999 );
    check_int( calendar, NumberOfYearsInThisEra, 9999 );
    check_int( calendar, Month, 2 );
    check_int( calendar, FirstMonthInThisYear, 1 );
    check_int( calendar, LastMonthInThisYear, 12 );
    check_int( calendar, NumberOfMonthsInThisYear, 12 );
    check_int( calendar, Day, 29 );
    check_int( calendar, FirstDayInThisMonth, 1 );
    check_int( calendar, LastDayInThisMonth, 29 );
    check_int( calendar, NumberOfDaysInThisMonth, 29 );
    check_int( calendar, Period, 1 );
    check_int( calendar, FirstPeriodInThisDay, 1 );
    check_int( calendar, LastPeriodInThisDay, 1 );
    check_int( calendar, NumberOfPeriodsInThisDay, 1 );
    check_int( calendar, Hour, 13 );
    check_int( calendar, FirstHourInThisPeriod, 0 );
    check_int( calendar, LastHourInThisPeriod, 23 );
    check_int( calendar, NumberOfHoursInThisPeriod, 24 );
    check_int( calendar, Minute, 45 );
    check_int( calendar, FirstMinuteInThisHour, 0 );
    check_int( calendar, LastMinuteInThisHour, 59 );
    check_int( calendar, NumberOfMinutesInThisHour, 60 );
    check_int( calendar, Second, 30 );
    check_int( calendar, FirstSecondInThisMinute, 0 );
    check_int( calendar, LastSecondInThisMinute, 59 );
    check_int( calendar, NumberOfSecondsInThisMinute, 60 );
    check_int( calendar, Nanosecond, 123456700 );

    hr = ICalendar_get_DayOfWeek( calendar, &day_of_week );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( day_of_week == DayOfWeek_Thursday, "got %d.\n", day_of_week );
    hr = ICalendar_get_IsDaylightSavingTime( calendar, &dst );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !dst, "got %d.\n", dst );

    check_str( calendar, YearAsString, L"2024" );
    check_str_arg( calendar, YearAsTruncatedString, 0, NULL );
    check_str_arg( calendar, YearAsTruncatedString, -1, NULL );
    check_str_arg( calendar, YearAsTruncatedString, 1, L"4" );
    check_str_arg( calendar, YearAsTruncatedString, 2, L"24" );
    check_str_arg( calendar, YearAsTruncatedString, 5, L"02024" );
    check_str_arg( calendar, YearAsPaddedString, 0, NULL );
    check_str_arg( calendar, YearAsPaddedString, 2, L"2024" );
    check_str_arg( calendar, YearAsPaddedString, 6, L"002024" );
    check_str( calendar, MonthAsFullString, L"February" );
    check_str( calendar, MonthAsFullSoloString, L"February" );
    check_str_arg( calendar, MonthAsString, 0, L"Feb" );
    check_str_arg( calendar, MonthAsString, 1, L"Feb" );
    check_str_arg( calendar, MonthAsString, 4, L"Feb" );
    check_str_arg( calendar, MonthAsString, 20, L"February" );
    check_str_arg( calendar, MonthAsSoloString, 3, L"Feb" );
    check_str( calendar, MonthAsNumericString, L"2" );
    check_str_arg( calendar, MonthAsPaddedNumericString, 0, NULL );
    check_str_arg( calendar, MonthAsPaddedNumericString, 2, L"02" );
    check_str_arg( calendar, MonthAsPaddedNumericString, 4, L"0002" );
    check_str( calendar, DayAsString, L"29" );
    check_str_arg( calendar, DayAsPaddedString, 3, L"029" );
    check_str( calendar, DayOfWeekAsFullString, L"Thursday" );
    check_str( calendar, DayOfWeekAsFullSoloString, L"Thursday" );
    check_str_arg( calendar, DayOfWeekAsString, 0, L"Thu" );
    check_str_arg( calendar, DayOfWeekAsString, 1, L"Th" );
    check_str_arg( calendar, DayOfWeekAsString, 2, L"Th" );
    check_str_arg( calendar, DayOfWeekAsString, 3, L"Thu" );
    check_str_arg( calendar, DayOfWeekAsString, 8, L"Thursday" );
    check_str_arg( calendar, DayOfWeekAsSoloString, 3, L"Thu" );
    check_str_arg( calendar, EraAsString, -1, NULL );
    check_str_arg( calendar, EraAsString, 1, L"AD" );
    check_str_arg( calendar, EraAsString, 3, L"AD" );
    check_str( calendar, PeriodAsFullString, L"" );
    check_str_arg( calendar, PeriodAsString, 2, L"" );
    check_str( calendar, HourAsString, L"13" );
    check_str_arg( calendar, HourAsPaddedString, 3, L"013" );
    check_str( calendar, MinuteAsString, L"45" );
    check_str_arg( calendar, MinuteAsPaddedString, 3, L"045" );
    check_str( calendar, SecondAsString, L"30" );
    check_str_arg( calendar, SecondAsPaddedString, 3, L"030" );
    check_str( calendar, NanosecondAsString, L"123456700" );
    check_str_arg( calendar, NanosecondAsPaddedString, 2, L"123456700" );
    check_str_arg( calendar, NanosecondAsPaddedString, 12, L"000123456700" );
    check_str( calendar, GetCalendarSystem, L"GregorianCalendar" );
    check_str( calendar, get_NumeralSystem, L"Latn" );

    /* the Gregorian era is "A.D." in en-US; Wine's locale data has "AD" */
    {
        HSTRING str;
        hr = ICalendar_EraAsFullString( calendar, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        todo_wine ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), L"A.D." ), "got %s.\n", dbg_hs( str ) );
        WindowsDeleteString( str );
    }

    /* 12-hour clock */
    ICalendar_Release( calendar );
    calendar = create_utc_calendar( L"12HourClock" );
    set_datetime( calendar, make_datetime( 2024, 2, 29, 13, 45, 30 ) );
    check_int( calendar, Period, 2 );
    check_int( calendar, Hour, 1 );
    check_int( calendar, FirstPeriodInThisDay, 1 );
    check_int( calendar, LastPeriodInThisDay, 2 );
    check_int( calendar, NumberOfPeriodsInThisDay, 2 );
    check_int( calendar, FirstHourInThisPeriod, 12 );
    check_int( calendar, LastHourInThisPeriod, 11 );
    check_int( calendar, NumberOfHoursInThisPeriod, 12 );
    check_str( calendar, PeriodAsFullString, L"PM" );
    check_str_arg( calendar, PeriodAsString, 0, L"PM" );
    check_str_arg( calendar, PeriodAsString, 1, L"P" );
    check_str_arg( calendar, PeriodAsString, 3, L"PM" );
    check_str( calendar, HourAsString, L"1" );
    set_datetime( calendar, make_datetime( 2024, 2, 29, 0, 30, 0 ) );
    check_int( calendar, Period, 1 );
    check_int( calendar, Hour, 12 );
    check_str( calendar, PeriodAsFullString, L"AM" );

    /* numeral systems */
    {
        static const struct { const WCHAR *name, *result, *year; } numerals[] =
        {
            {L"Latn", L"Latn", L"2024"},
            {L"latn", L"Latn", L"2024"},
            {L"Arab", L"Arab", L"\x0662\x0660\x0662\x0664"},
            {L"Foo", NULL},
            {L"", NULL},
        };
        unsigned int i;
        HSTRING str;

        for (i = 0; i < ARRAY_SIZE(numerals); i++)
        {
            winetest_push_context( "%u", i );
            str = hs( numerals[i].name );
            hr = ICalendar_put_NumeralSystem( calendar, str );
            WindowsDeleteString( str );
            ok( hr == (numerals[i].result ? S_OK : E_INVALIDARG), "got hr %#lx.\n", hr );
            if (numerals[i].result)
            {
                check_str( calendar, get_NumeralSystem, numerals[i].result );
                check_str( calendar, YearAsString, numerals[i].year );
            }
            winetest_pop_context();
        }
    }

    ICalendar_Release( calendar );
}

static void test_Calendar_limits(void)
{
    ICalendar *calendar = create_utc_calendar( L"24HourClock" );
    DateTime dt;
    HRESULT hr;

    hr = ICalendar_SetToMin( calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( get_datetime( calendar ) == -504911232000000000, "got %I64d.\n", get_datetime( calendar ) );
    check_int( calendar, Year, 1 );
    check_int( calendar, Month, 1 );
    check_int( calendar, Day, 1 );
    check_int( calendar, Nanosecond, 0 );

    hr = ICalendar_SetToMax( calendar );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( get_datetime( calendar ) == 2650467743999999999, "got %I64d.\n", get_datetime( calendar ) );
    check_int( calendar, Year, 9999 );
    check_int( calendar, Month, 12 );
    check_int( calendar, Day, 31 );
    check_int( calendar, Hour, 23 );
    check_int( calendar, Nanosecond, 999999999 );

    set_datetime( calendar, 0 );
    check_int( calendar, Year, 1601 );
    check_int( calendar, Month, 1 );
    check_int( calendar, Day, 1 );
    set_datetime( calendar, -1 );
    check_int( calendar, Year, 1600 );
    check_int( calendar, Day, 31 );
    check_int( calendar, Nanosecond, 999999900 );

    dt.UniversalTime = -504911232000000000 - 1;
    hr = ICalendar_SetDateTime( calendar, dt );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( get_datetime( calendar ) == -1, "got %I64d.\n", get_datetime( calendar ) );
    dt.UniversalTime = 0x7fffffffffffffff;
    hr = ICalendar_SetDateTime( calendar, dt );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    /* nanoseconds are kept below the resolution of DateTime */
    set_datetime( calendar, make_datetime( 2024, 2, 29, 10, 0, 0 ) );
    hr = ICalendar_put_Nanosecond( calendar, 123456789 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_int( calendar, Nanosecond, 123456789 );
    ok( get_datetime( calendar ) == make_datetime( 2024, 2, 29, 10, 0, 0 ) + 1234567, "got %I64d.\n",
        get_datetime( calendar ) );
    set_datetime( calendar, make_datetime( 2024, 2, 29, 10, 0, 0 ) );
    hr = ICalendar_AddNanoseconds( calendar, 150 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_int( calendar, Nanosecond, 150 );
    set_datetime( calendar, make_datetime( 2024, 2, 29, 10, 0, 0 ) );
    hr = ICalendar_AddNanoseconds( calendar, -1 );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_int( calendar, Nanosecond, 999999999 );
    ok( get_datetime( calendar ) == make_datetime( 2024, 2, 29, 10, 0, 0 ) - 1, "got %I64d.\n",
        get_datetime( calendar ) );

    ICalendar_Release( calendar );
}

enum op
{
    OP_PUT_ERA, OP_PUT_YEAR, OP_PUT_MONTH, OP_PUT_DAY, OP_PUT_PERIOD, OP_PUT_HOUR, OP_PUT_MINUTE, OP_PUT_SECOND,
    OP_PUT_NANOSECOND, OP_ADD_ERAS, OP_ADD_YEARS, OP_ADD_MONTHS, OP_ADD_WEEKS, OP_ADD_DAYS, OP_ADD_PERIODS,
    OP_ADD_HOURS, OP_ADD_MINUTES, OP_ADD_SECONDS, OP_ADD_NANOSECONDS,
};

static HRESULT apply_op( ICalendar *calendar, enum op op, INT32 value )
{
    switch (op)
    {
    case OP_PUT_ERA: return ICalendar_put_Era( calendar, value );
    case OP_PUT_YEAR: return ICalendar_put_Year( calendar, value );
    case OP_PUT_MONTH: return ICalendar_put_Month( calendar, value );
    case OP_PUT_DAY: return ICalendar_put_Day( calendar, value );
    case OP_PUT_PERIOD: return ICalendar_put_Period( calendar, value );
    case OP_PUT_HOUR: return ICalendar_put_Hour( calendar, value );
    case OP_PUT_MINUTE: return ICalendar_put_Minute( calendar, value );
    case OP_PUT_SECOND: return ICalendar_put_Second( calendar, value );
    case OP_PUT_NANOSECOND: return ICalendar_put_Nanosecond( calendar, value );
    case OP_ADD_ERAS: return ICalendar_AddEras( calendar, value );
    case OP_ADD_YEARS: return ICalendar_AddYears( calendar, value );
    case OP_ADD_MONTHS: return ICalendar_AddMonths( calendar, value );
    case OP_ADD_WEEKS: return ICalendar_AddWeeks( calendar, value );
    case OP_ADD_DAYS: return ICalendar_AddDays( calendar, value );
    case OP_ADD_PERIODS: return ICalendar_AddPeriods( calendar, value );
    case OP_ADD_HOURS: return ICalendar_AddHours( calendar, value );
    case OP_ADD_MINUTES: return ICalendar_AddMinutes( calendar, value );
    case OP_ADD_SECONDS: return ICalendar_AddSeconds( calendar, value );
    case OP_ADD_NANOSECONDS: return ICalendar_AddNanoseconds( calendar, value );
    }
    return E_FAIL;
}

static void test_Calendar_arithmetic(void)
{
    /* Starting points are UTC; in Europe/Berlin daylight time starts on 2024-03-31 at 01:00Z
     * (02:00 local) and ends on 2024-10-27 at 01:00Z (03:00 local). */
    static const struct
    {
        BOOL twelve_hour;
        const WCHAR *time_zone;
        struct { int year, month, day, hour, minute, second; } start;
        enum op op;
        INT32 value;
        HRESULT hr;
        INT64 result;
        INT32 hour, period;
    }
    tests[] =
    {
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_PUT_MONTH, 2, S_OK, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_PUT_MONTH, 4, S_OK, 133589448000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_PUT_MONTH, 0, E_INVALIDARG, 133511688000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_PUT_MONTH, 13, E_INVALIDARG, 133511688000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_PUT_MONTH, -1, E_INVALIDARG, 133511688000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 4, 15, 10, 0, 0}, OP_PUT_DAY, 31, E_INVALIDARG, 133576488000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 4, 15, 10, 0, 0}, OP_PUT_DAY, 30, S_OK, 133589448000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 4, 15, 10, 0, 0}, OP_PUT_DAY, 0, E_INVALIDARG, 133576488000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 2023, S_OK, 133220520000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 2028, S_OK, 134799048000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 0, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 1, S_OK, -504860760000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 1600, S_OK, -264888000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 9999, S_OK, 2650202856000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_YEAR, 10000, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_ERA, 1, S_OK, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_ERA, 2, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_ERA, 0, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_HOUR, 0, S_OK, 133536384000000000ll, 0, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_HOUR, 23, S_OK, 133537212000000000ll, 23, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_HOUR, 24, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_HOUR, -1, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_MINUTE, 60, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_MINUTE, 59, S_OK, 133536779400000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_SECOND, 60, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_NANOSECOND, 123456789, S_OK, 133536744001234567ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_NANOSECOND, 999999999, S_OK, 133536744009999999ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_NANOSECOND, 1000000000, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_NANOSECOND, -1, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_PERIOD, 1, S_OK, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_PUT_PERIOD, 2, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_PERIODS, 1, S_OK, 133537608000000000ll, 10, 1},
        {TRUE , L"UTC", {2024, 2, 29, 0, 30, 0}, OP_PUT_HOUR, 12, S_OK, 133536402000000000ll, 12, 1},
        {TRUE , L"UTC", {2024, 2, 29, 0, 30, 0}, OP_PUT_HOUR, 1, S_OK, 133536438000000000ll, 1, 1},
        {TRUE , L"UTC", {2024, 2, 29, 0, 30, 0}, OP_PUT_HOUR, 0, E_INVALIDARG, 133536402000000000ll, 12, 1},
        {TRUE , L"UTC", {2024, 2, 29, 0, 30, 0}, OP_PUT_HOUR, 13, E_INVALIDARG, 133536402000000000ll, 12, 1},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_PUT_HOUR, 12, S_OK, 133536834000000000ll, 12, 2},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_PUT_HOUR, 5, S_OK, 133537014000000000ll, 5, 2},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_PUT_PERIOD, 1, S_OK, 133536474000000000ll, 2, 1},
        {TRUE , L"UTC", {2024, 2, 29, 2, 30, 0}, OP_PUT_PERIOD, 2, S_OK, 133536906000000000ll, 2, 2},
        {TRUE , L"UTC", {2024, 2, 29, 2, 30, 0}, OP_PUT_PERIOD, 3, E_INVALIDARG, 133536474000000000ll, 2, 1},
        {TRUE , L"UTC", {2024, 2, 29, 2, 30, 0}, OP_PUT_PERIOD, 0, E_INVALIDARG, 133536474000000000ll, 2, 1},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_ADD_PERIODS, 1, S_OK, 133537338000000000ll, 2, 1},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_ADD_PERIODS, 3, S_OK, 133538202000000000ll, 2, 1},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_ADD_PERIODS, -1, S_OK, 133536474000000000ll, 2, 1},
        {TRUE , L"UTC", {2024, 2, 29, 14, 30, 0}, OP_ADD_HOURS, 12, S_OK, 133537338000000000ll, 2, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_ADD_MONTHS, 1, S_OK, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_ADD_MONTHS, 13, S_OK, 133852104000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 1, 31, 10, 0, 0}, OP_ADD_MONTHS, -2, S_OK, 133458120000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 3, 31, 10, 0, 0}, OP_ADD_MONTHS, -1, S_OK, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_YEARS, 1, S_OK, 133852104000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_YEARS, 4, S_OK, 134799048000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_YEARS, -2030, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_YEARS, 8000, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_ERAS, 1, E_INVALIDARG, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_ERAS, 0, S_OK, 133536744000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_WEEKS, 1, S_OK, 133542792000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_DAYS, 1, S_OK, 133537608000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_DAYS, -365, S_OK, 133221384000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_HOURS, 14, S_OK, 133537248000000000ll, 0, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_MINUTES, -601, S_OK, 133536383400000000ll, 23, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_SECONDS, 86400, S_OK, 133537608000000000ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_NANOSECONDS, 150, S_OK, 133536744000000001ll, 10, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_NANOSECONDS, -1, S_OK, 133536743999999999ll, 9, 1},
        {FALSE, L"UTC", {2024, 2, 29, 10, 0, 0}, OP_ADD_NANOSECONDS, 2000000000, S_OK, 133536744020000000ll, 10, 1},
        {FALSE, L"Europe/Berlin", {2024, 3, 31, 0, 30, 0}, OP_ADD_HOURS, 1, S_OK, 133563222000000000ll, 3, 1},
        {FALSE, L"Europe/Berlin", {2024, 3, 31, 0, 30, 0}, OP_ADD_MINUTES, 60, S_OK, 133563222000000000ll, 3, 1},
        {FALSE, L"Europe/Berlin", {2024, 3, 30, 1, 30, 0}, OP_ADD_DAYS, 1, S_OK, 133563222000000000ll, 3, 1},
        {FALSE, L"Europe/Berlin", {2024, 3, 31, 0, 30, 0}, OP_PUT_HOUR, 2, E_INVALIDARG, 133563186000000000ll, 1, 1},
        {FALSE, L"Europe/Berlin", {2024, 3, 31, 0, 30, 0}, OP_PUT_HOUR, 3, S_OK, 133563222000000000ll, 3, 1},
        {FALSE, L"Europe/Berlin", {2024, 10, 27, 0, 30, 0}, OP_ADD_HOURS, 1, S_OK, 133744662000000000ll, 2, 1},
        {FALSE, L"Europe/Berlin", {2024, 10, 27, 0, 30, 0}, OP_PUT_HOUR, 2, S_OK, 133744626000000000ll, 2, 1},
        {FALSE, L"Europe/Berlin", {2024, 10, 27, 1, 30, 0}, OP_PUT_HOUR, 2, S_OK, 133744662000000000ll, 2, 1},
        {FALSE, L"Europe/Berlin", {2024, 10, 26, 0, 30, 0}, OP_ADD_DAYS, 1, S_OK, 133744626000000000ll, 2, 1},
        {FALSE, L"Europe/Berlin", {2024, 10, 27, 0, 30, 0}, OP_PUT_MINUTE, 45, S_OK, 133744635000000000ll, 2, 1},
        {FALSE, L"Europe/Berlin", {2024, 10, 27, 1, 30, 0}, OP_PUT_MINUTE, 45, S_OK, 133744671000000000ll, 2, 1},
    };
    ICalendar *calendar;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        winetest_push_context( "%u", i );
        hr = create_calendar( L"en-US", L"GregorianCalendar", tests[i].twelve_hour ? L"12HourClock" : L"24HourClock",
                              tests[i].time_zone, &calendar );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        set_datetime( calendar, make_datetime( tests[i].start.year, tests[i].start.month, tests[i].start.day,
                                               tests[i].start.hour, tests[i].start.minute, tests[i].start.second ) );
        hr = apply_op( calendar, tests[i].op, tests[i].value );
        ok( hr == tests[i].hr, "got hr %#lx.\n", hr );
        ok( get_datetime( calendar ) == tests[i].result, "got %I64d, expected %I64d.\n", get_datetime( calendar ),
            tests[i].result );
        check_int( calendar, Hour, tests[i].hour );
        check_int( calendar, Period, tests[i].period );
        ICalendar_Release( calendar );
        winetest_pop_context();
    }
}

static void test_Calendar_time_zone(void)
{
    static const struct
    {
        const WCHAR *id;
        HRESULT hr;
        const WCHAR *resolved;
        INT32 hour;
        BOOL dst;
        const WCHAR *name;
        BOOL todo_name;
    }
    tests[] =
    {
        /* at 2024-07-01T12:00Z */
        {L"UTC", S_OK, L"Etc/UTC", 12, FALSE, L"UTC"},
        {L"Etc/GMT", S_OK, L"Etc/GMT", 12, FALSE, L"GMT"},
        {L"Europe/Berlin", S_OK, L"Europe/Berlin", 14, TRUE, L"GMT+2"},
        {L"America/New_York", S_OK, L"America/New_York", 8, TRUE, L"EDT", TRUE},
        {L"Asia/Kolkata", S_OK, L"Asia/Calcutta", 17, FALSE, L"GMT+5:30"},
        {L"Australia/Lord_Howe", S_OK, L"Australia/Lord_Howe", 22, FALSE, L"GMT+10:30"},
        {L"W. Europe Standard Time", E_INVALIDARG},
        {L"Invalid/Zone", E_INVALIDARG},
        {L"", E_INVALIDARG},
    };
    ICalendar *calendar = create_utc_calendar( L"24HourClock" ), *other;
    ITimeZoneOnCalendar *time_zone;
    unsigned int i;
    boolean dst;
    HSTRING str;
    HRESULT hr;

    hr = ICalendar_QueryInterface( calendar, &IID_ITimeZoneOnCalendar, (void **)&time_zone );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = ITimeZoneOnCalendar_TimeZoneAsFullString( time_zone, &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_hstring( str, L"Coordinated Universal Time" );

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        winetest_push_context( "%u", i );
        hr = ITimeZoneOnCalendar_ChangeTimeZone( time_zone, (str = hs( L"Etc/UTC" )) );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        WindowsDeleteString( str );
        set_datetime( calendar, make_datetime( 2024, 7, 1, 12, 0, 0 ) );

        hr = ITimeZoneOnCalendar_ChangeTimeZone( time_zone, (str = hs( tests[i].id )) );
        WindowsDeleteString( str );
        ok( hr == tests[i].hr, "got hr %#lx.\n", hr );
        if (FAILED(hr))
        {
            /* a failed change keeps the previous time zone */
            hr = ITimeZoneOnCalendar_GetTimeZone( time_zone, &str );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            check_hstring( str, L"Etc/UTC" );
            winetest_pop_context();
            continue;
        }
        hr = ITimeZoneOnCalendar_GetTimeZone( time_zone, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        check_hstring( str, tests[i].resolved );
        ok( get_datetime( calendar ) == make_datetime( 2024, 7, 1, 12, 0, 0 ), "time changed.\n" );
        check_int( calendar, Hour, tests[i].hour );
        hr = ICalendar_get_IsDaylightSavingTime( calendar, &dst );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( dst == tests[i].dst, "got %d.\n", dst );
        hr = ITimeZoneOnCalendar_TimeZoneAsString( time_zone, 3, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        todo_wine_if( tests[i].todo_name )
        ok( !wcscmp( WindowsGetStringRawBuffer( str, NULL ), tests[i].name ), "got %s.\n", dbg_hs( str ) );
        WindowsDeleteString( str );
        winetest_pop_context();
    }

    /* Clone and CopyTo take the time zone along */
    hr = ITimeZoneOnCalendar_ChangeTimeZone( time_zone, (str = hs( L"Asia/Kolkata" )) );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = ICalendar_Clone( calendar, &other );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( other != calendar, "got same object.\n" );
    check_int( other, Hour, 17 );
    ICalendar_Release( other );

    other = create_utc_calendar( L"12HourClock" );
    hr = ICalendar_CopyTo( calendar, other );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    check_int( other, Hour, 17 );
    check_str( other, GetClock, L"24HourClock" );
    hr = ICalendar_CopyTo( calendar, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    ICalendar_Release( other );

    ITimeZoneOnCalendar_Release( time_zone );
    ICalendar_Release( calendar );
}

static void test_Calendar_compare(void)
{
    ICalendar *calendar = create_utc_calendar( L"24HourClock" ), *other = create_utc_calendar( L"24HourClock" );
    DateTime dt;
    INT32 result;
    HRESULT hr;

    set_datetime( calendar, make_datetime( 2024, 2, 29, 10, 0, 0 ) );
    set_datetime( other, make_datetime( 2024, 2, 29, 10, 0, 0 ) + 1 );
    hr = ICalendar_Compare( calendar, other, &result );
    ok( hr == S_OK && result == -1, "got hr %#lx, result %d.\n", hr, result );
    hr = ICalendar_Compare( other, calendar, &result );
    ok( hr == S_OK && result == 1, "got hr %#lx, result %d.\n", hr, result );
    hr = ICalendar_Compare( calendar, calendar, &result );
    ok( hr == S_OK && result == 0, "got hr %#lx, result %d.\n", hr, result );
    result = 42;
    hr = ICalendar_Compare( calendar, NULL, &result );
    ok( hr == E_POINTER && result == 0, "got hr %#lx, result %d.\n", hr, result );

    dt.UniversalTime = make_datetime( 2025, 1, 1, 0, 0, 0 );
    hr = ICalendar_CompareDateTime( calendar, dt, &result );
    ok( hr == S_OK && result == -1, "got hr %#lx, result %d.\n", hr, result );
    dt.UniversalTime = make_datetime( 2000, 1, 1, 0, 0, 0 );
    hr = ICalendar_CompareDateTime( calendar, dt, &result );
    ok( hr == S_OK && result == 1, "got hr %#lx, result %d.\n", hr, result );

    ICalendar_Release( other );
    ICalendar_Release( calendar );
}

static void test_Calendar_systems(void)
{
    static const struct { const WCHAR *name; HRESULT hr; BOOL todo; } systems[] =
    {
        {L"GregorianCalendar", S_OK},
        {L"JapaneseCalendar", S_OK, TRUE},
        {L"HebrewCalendar", S_OK, TRUE},
        {L"gregoriancalendar", E_INVALIDARG},
        {L"Foo", E_INVALIDARG},
        {L"", E_INVALIDARG},
    };
    static const struct { const WCHAR *name; HRESULT hr; const WCHAR *result; } clocks[] =
    {
        {L"12HourClock", S_OK, L"12HourClock"},
        {L"24HourClock", S_OK, L"24HourClock"},
        {L"24hourclock", E_INVALIDARG, L"24HourClock"},
        {L"Foo", E_INVALIDARG, L"24HourClock"},
    };
    ICalendar *calendar = create_utc_calendar( L"24HourClock" );
    unsigned int i;
    HSTRING str;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(systems); i++)
    {
        winetest_push_context( "%u", i );
        hr = ICalendar_ChangeCalendarSystem( calendar, (str = hs( systems[i].name )) );
        WindowsDeleteString( str );
        todo_wine_if( systems[i].todo )
        ok( hr == systems[i].hr, "got hr %#lx.\n", hr );
        hr = ICalendar_ChangeCalendarSystem( calendar, (str = hs( L"GregorianCalendar" )) );
        WindowsDeleteString( str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        winetest_pop_context();
    }

    for (i = 0; i < ARRAY_SIZE(clocks); i++)
    {
        winetest_push_context( "%u", i );
        hr = ICalendar_ChangeClock( calendar, (str = hs( clocks[i].name )) );
        WindowsDeleteString( str );
        ok( hr == clocks[i].hr, "got hr %#lx.\n", hr );
        check_str( calendar, GetClock, clocks[i].result );
        winetest_pop_context();
    }

    ICalendar_Release( calendar );
}

START_TEST(calendar)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_Calendar_activation();
    test_Calendar_factory();
    test_Calendar_fields();
    test_Calendar_limits();
    test_Calendar_arithmetic();
    test_Calendar_time_zone();
    test_Calendar_compare();
    test_Calendar_systems();

    RoUninitialize();
}
