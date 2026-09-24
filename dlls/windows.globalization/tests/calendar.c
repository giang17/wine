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
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winstring.h"

#include "initguid.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Globalization
#include "windows.globalization.h"

#include "wine/test.h"

/* PROBE: dumps the behaviour of Windows.Globalization.Calendar as "P|key|value" lines. */

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
    return wine_dbgstr_w( WindowsGetStringRawBuffer( str, NULL ) );
}

static INT64 make_datetime( int year, int month, int day, int hour, int minute, int second, int ticks )
{
    SYSTEMTIME st = {year, month, 0, day, hour, minute, second, 0};
    FILETIME ft;
    SystemTimeToFileTime( &st, &ft );
    return (((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + ticks;
}

static const char *dbg_datetime( INT64 value )
{
    SYSTEMTIME st;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)value;
    ft.dwHighDateTime = (DWORD)(value >> 32);
    if (!FileTimeToSystemTime( &ft, &st )) return wine_dbg_sprintf( "%I64d(invalid)", value );
    return wine_dbg_sprintf( "%I64d(%04u-%02u-%02uT%02u:%02u:%02u.%07uZ)", value, st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond, (UINT)(value % 10000000) );
}

static const char *dbg_hr_hs( HRESULT hr, HSTRING str )
{
    if (FAILED(hr)) return wine_dbg_sprintf( "hr=%#lx", hr );
    return wine_dbg_sprintf( "%s", dbg_hs( str ) );
}



static void dump_int( const char *tag, const char *key, HRESULT hr, INT32 value )
{
    if (FAILED(hr)) trace( "P|%s|%s|hr=%#lx\n", tag, key, hr );
    else trace( "P|%s|%s|%d\n", tag, key, value );
}

#define DUMP_GET(name) do { INT32 v = -12345; HRESULT h = ICalendar_get_##name( cal, &v ); dump_int( tag, #name, h, v ); } while (0)

#define DUMP_STR0(name) do { HSTRING s = NULL; HRESULT h = ICalendar_##name( cal, &s ); \
        trace( "P|%s|%s|%s\n", tag, #name, dbg_hr_hs( h, s ) ); WindowsDeleteString( s ); } while (0)

#define DUMP_STR1(name, arg) do { HSTRING s = NULL; HRESULT h = ICalendar_##name( cal, arg, &s ); \
        trace( "P|%s|%s(%d)|%s\n", tag, #name, arg, dbg_hr_hs( h, s ) ); WindowsDeleteString( s ); } while (0)

static void dump_datetime( ICalendar *cal, const char *tag )
{
    DateTime dt = {0};
    HRESULT hr = ICalendar_GetDateTime( cal, &dt );
    if (FAILED(hr)) trace( "P|%s|DateTime|hr=%#lx\n", tag, hr );
    else trace( "P|%s|DateTime|%s\n", tag, dbg_datetime( dt.UniversalTime ) );
}

static void dump_fields( ICalendar *cal, const char *tag )
{
    DayOfWeek dow = -1;
    HRESULT hr;

    dump_datetime( cal, tag );
    DUMP_GET(Era); DUMP_GET(Year); DUMP_GET(Month); DUMP_GET(Day);
    DUMP_GET(Period); DUMP_GET(Hour); DUMP_GET(Minute); DUMP_GET(Second); DUMP_GET(Nanosecond);
    hr = ICalendar_get_DayOfWeek( cal, &dow );
    dump_int( tag, "DayOfWeek", hr, dow );
}

static void dump_all( ICalendar *cal, const char *tag )
{
    IVectorView_HSTRING *languages = NULL;
    ITimeZoneOnCalendar *tz = NULL;
    boolean dst = 2;
    UINT32 size = 0, i;
    HRESULT hr;
    HSTRING s;

    dump_fields( cal, tag );

    DUMP_GET(FirstEra); DUMP_GET(LastEra); DUMP_GET(NumberOfEras);
    DUMP_GET(FirstYearInThisEra); DUMP_GET(LastYearInThisEra); DUMP_GET(NumberOfYearsInThisEra);
    DUMP_GET(FirstMonthInThisYear); DUMP_GET(LastMonthInThisYear); DUMP_GET(NumberOfMonthsInThisYear);
    DUMP_GET(FirstDayInThisMonth); DUMP_GET(LastDayInThisMonth); DUMP_GET(NumberOfDaysInThisMonth);
    DUMP_GET(FirstPeriodInThisDay); DUMP_GET(LastPeriodInThisDay); DUMP_GET(NumberOfPeriodsInThisDay);
    DUMP_GET(FirstHourInThisPeriod); DUMP_GET(LastHourInThisPeriod); DUMP_GET(NumberOfHoursInThisPeriod);
    DUMP_GET(FirstMinuteInThisHour); DUMP_GET(LastMinuteInThisHour); DUMP_GET(NumberOfMinutesInThisHour);
    DUMP_GET(FirstSecondInThisMinute); DUMP_GET(LastSecondInThisMinute); DUMP_GET(NumberOfSecondsInThisMinute);

    hr = ICalendar_get_IsDaylightSavingTime( cal, &dst );
    dump_int( tag, "IsDaylightSavingTime", hr, dst );

    hr = ICalendar_get_Languages( cal, &languages );
    if (FAILED(hr)) trace( "P|%s|Languages|hr=%#lx\n", tag, hr );
    else
    {
        IVectorView_HSTRING_get_Size( languages, &size );
        for (i = 0; i < size; i++)
        {
            IVectorView_HSTRING_GetAt( languages, i, &s );
            trace( "P|%s|Languages[%u]|%s\n", tag, i, dbg_hs( s ) );
            WindowsDeleteString( s );
        }
        trace( "P|%s|Languages.Size|%u\n", tag, size );
        IVectorView_HSTRING_Release( languages );
    }

    s = NULL; hr = ICalendar_get_ResolvedLanguage( cal, &s );
    trace( "P|%s|ResolvedLanguage|%s\n", tag, dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
    s = NULL; hr = ICalendar_get_NumeralSystem( cal, &s );
    trace( "P|%s|NumeralSystem|%s\n", tag, dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
    DUMP_STR0(GetCalendarSystem);
    DUMP_STR0(GetClock);

    DUMP_STR0(EraAsFullString); DUMP_STR1(EraAsString, 0); DUMP_STR1(EraAsString, 1);
    DUMP_STR1(EraAsString, 2); DUMP_STR1(EraAsString, 3); DUMP_STR1(EraAsString, 4); DUMP_STR1(EraAsString, -1);
    DUMP_STR0(YearAsString);
    DUMP_STR1(YearAsTruncatedString, 0); DUMP_STR1(YearAsTruncatedString, 1); DUMP_STR1(YearAsTruncatedString, 2);
    DUMP_STR1(YearAsTruncatedString, 5); DUMP_STR1(YearAsTruncatedString, -1);
    DUMP_STR1(YearAsPaddedString, 0); DUMP_STR1(YearAsPaddedString, 2); DUMP_STR1(YearAsPaddedString, 6);
    DUMP_STR1(YearAsPaddedString, -1);
    DUMP_STR0(MonthAsFullString); DUMP_STR1(MonthAsString, 0); DUMP_STR1(MonthAsString, 1);
    DUMP_STR1(MonthAsString, 2); DUMP_STR1(MonthAsString, 3); DUMP_STR1(MonthAsString, 4); DUMP_STR1(MonthAsString, 20);
    DUMP_STR0(MonthAsFullSoloString); DUMP_STR1(MonthAsSoloString, 3);
    DUMP_STR0(MonthAsNumericString); DUMP_STR1(MonthAsPaddedNumericString, 0);
    DUMP_STR1(MonthAsPaddedNumericString, 2); DUMP_STR1(MonthAsPaddedNumericString, 4);
    DUMP_STR0(DayAsString); DUMP_STR1(DayAsPaddedString, 2); DUMP_STR1(DayAsPaddedString, 3);
    DUMP_STR0(DayOfWeekAsFullString); DUMP_STR1(DayOfWeekAsString, 0); DUMP_STR1(DayOfWeekAsString, 1);
    DUMP_STR1(DayOfWeekAsString, 2); DUMP_STR1(DayOfWeekAsString, 3); DUMP_STR1(DayOfWeekAsString, 4);
    DUMP_STR0(DayOfWeekAsFullSoloString); DUMP_STR1(DayOfWeekAsSoloString, 3);
    DUMP_STR0(PeriodAsFullString); DUMP_STR1(PeriodAsString, 0); DUMP_STR1(PeriodAsString, 1);
    DUMP_STR1(PeriodAsString, 2); DUMP_STR1(PeriodAsString, 3);
    DUMP_STR0(HourAsString); DUMP_STR1(HourAsPaddedString, 2); DUMP_STR1(HourAsPaddedString, 3);
    DUMP_STR0(MinuteAsString); DUMP_STR1(MinuteAsPaddedString, 2);
    DUMP_STR0(SecondAsString); DUMP_STR1(SecondAsPaddedString, 2);
    DUMP_STR0(NanosecondAsString); DUMP_STR1(NanosecondAsPaddedString, 2); DUMP_STR1(NanosecondAsPaddedString, 9);
    DUMP_STR1(NanosecondAsPaddedString, 12);

    hr = ICalendar_QueryInterface( cal, &IID_ITimeZoneOnCalendar, (void **)&tz );
    if (FAILED(hr)) trace( "P|%s|ITimeZoneOnCalendar|hr=%#lx\n", tag, hr );
    else
    {
        s = NULL; hr = ITimeZoneOnCalendar_GetTimeZone( tz, &s );
        trace( "P|%s|GetTimeZone|%s\n", tag, dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
        s = NULL; hr = ITimeZoneOnCalendar_TimeZoneAsFullString( tz, &s );
        trace( "P|%s|TimeZoneAsFullString|%s\n", tag, dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
        for (i = 0; i < 6; i++)
        {
            s = NULL; hr = ITimeZoneOnCalendar_TimeZoneAsString( tz, i, &s );
            trace( "P|%s|TimeZoneAsString(%u)|%s\n", tag, i, dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
        }
        ITimeZoneOnCalendar_Release( tz );
    }
}

static IActivationFactory *get_factory(void)
{
    static const WCHAR *name = L"Windows.Globalization.Calendar";
    IActivationFactory *factory = NULL;
    HSTRING str = hs( name );
    HRESULT hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK, "RoGetActivationFactory failed, hr %#lx\n", hr );
    return factory;
}

static ICalendar *create_calendar( const WCHAR *lang, const WCHAR *system, const WCHAR *clock, const WCHAR *tz,
                                   HRESULT *ret )
{
    ICalendarFactory2 *factory2 = NULL;
    ICalendarFactory *factory1 = NULL;
    IActivationFactory *factory = get_factory();
    IIterable_HSTRING *langs = NULL;
    ICalendar *cal = NULL;
    HSTRING hsystem = hs( system ), hclock = hs( clock ), htz = hs( tz );
    HRESULT hr;

    if (lang) langs = create_languages( &lang, 1 );
    if (tz)
    {
        IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory2, (void **)&factory2 );
        hr = ICalendarFactory2_CreateCalendarWithTimeZone( factory2, langs, hsystem, hclock, htz, &cal );
        ICalendarFactory2_Release( factory2 );
    }
    else
    {
        IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory, (void **)&factory1 );
        if (system) hr = ICalendarFactory_CreateCalendar( factory1, langs, hsystem, hclock, &cal );
        else hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, langs, &cal );
        ICalendarFactory_Release( factory1 );
    }
    if (langs) IIterable_HSTRING_Release( langs );
    WindowsDeleteString( hsystem ); WindowsDeleteString( hclock ); WindowsDeleteString( htz );
    IActivationFactory_Release( factory );
    *ret = hr;
    return SUCCEEDED(hr) ? cal : NULL;
}

static void set_datetime( ICalendar *cal, INT64 value )
{
    DateTime dt = {value};
    HRESULT hr = ICalendar_SetDateTime( cal, dt );
    ok( hr == S_OK, "SetDateTime failed, hr %#lx\n", hr );
}

enum op
{
    OP_PUT_ERA, OP_PUT_YEAR, OP_PUT_MONTH, OP_PUT_DAY, OP_PUT_PERIOD, OP_PUT_HOUR, OP_PUT_MINUTE, OP_PUT_SECOND,
    OP_PUT_NANOSECOND, OP_ADD_ERAS, OP_ADD_YEARS, OP_ADD_MONTHS, OP_ADD_WEEKS, OP_ADD_DAYS, OP_ADD_PERIODS,
    OP_ADD_HOURS, OP_ADD_MINUTES, OP_ADD_SECONDS, OP_ADD_NANOSECONDS,
};

static const char *op_names[] =
{
    "put_Era", "put_Year", "put_Month", "put_Day", "put_Period", "put_Hour", "put_Minute", "put_Second",
    "put_Nanosecond", "AddEras", "AddYears", "AddMonths", "AddWeeks", "AddDays", "AddPeriods",
    "AddHours", "AddMinutes", "AddSeconds", "AddNanoseconds",
};

static HRESULT apply_op( ICalendar *cal, enum op op, INT32 value )
{
    switch (op)
    {
    case OP_PUT_ERA: return ICalendar_put_Era( cal, value );
    case OP_PUT_YEAR: return ICalendar_put_Year( cal, value );
    case OP_PUT_MONTH: return ICalendar_put_Month( cal, value );
    case OP_PUT_DAY: return ICalendar_put_Day( cal, value );
    case OP_PUT_PERIOD: return ICalendar_put_Period( cal, value );
    case OP_PUT_HOUR: return ICalendar_put_Hour( cal, value );
    case OP_PUT_MINUTE: return ICalendar_put_Minute( cal, value );
    case OP_PUT_SECOND: return ICalendar_put_Second( cal, value );
    case OP_PUT_NANOSECOND: return ICalendar_put_Nanosecond( cal, value );
    case OP_ADD_ERAS: return ICalendar_AddEras( cal, value );
    case OP_ADD_YEARS: return ICalendar_AddYears( cal, value );
    case OP_ADD_MONTHS: return ICalendar_AddMonths( cal, value );
    case OP_ADD_WEEKS: return ICalendar_AddWeeks( cal, value );
    case OP_ADD_DAYS: return ICalendar_AddDays( cal, value );
    case OP_ADD_PERIODS: return ICalendar_AddPeriods( cal, value );
    case OP_ADD_HOURS: return ICalendar_AddHours( cal, value );
    case OP_ADD_MINUTES: return ICalendar_AddMinutes( cal, value );
    case OP_ADD_SECONDS: return ICalendar_AddSeconds( cal, value );
    case OP_ADD_NANOSECONDS: return ICalendar_AddNanoseconds( cal, value );
    }
    return E_FAIL;
}

struct op_test
{
    const WCHAR *clock;
    const WCHAR *tz;
    INT64 start; /* filled at runtime from the fields below */
    int y, mo, d, h, mi, s, ticks;
    enum op op;
    INT32 value;
};

static void probe_ops(void)
{
    static struct op_test tests[] =
    {
        /* field puts, UTC, 24h */
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_PUT_MONTH, 2},
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_PUT_MONTH, 4},
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_PUT_MONTH, 0},
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_PUT_MONTH, 13},
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_PUT_MONTH, -1},
        {L"24HourClock", L"UTC", 0, 2024, 4, 15, 10, 0, 0, 0, OP_PUT_DAY, 31},
        {L"24HourClock", L"UTC", 0, 2024, 4, 15, 10, 0, 0, 0, OP_PUT_DAY, 30},
        {L"24HourClock", L"UTC", 0, 2024, 4, 15, 10, 0, 0, 0, OP_PUT_DAY, 0},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 2023},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 2028},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 0},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 1600},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 9999},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_YEAR, 10000},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_ERA, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_ERA, 2},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_ERA, 0},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_HOUR, 0},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_HOUR, 23},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_HOUR, 24},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_HOUR, -1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_MINUTE, 60},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_MINUTE, 59},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_SECOND, 60},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_NANOSECOND, 123456789},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_NANOSECOND, 999999999},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_NANOSECOND, 1000000000},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_NANOSECOND, -1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_PERIOD, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_PUT_PERIOD, 2},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_PERIODS, 1},
        /* 12h clock */
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 0, 30, 0, 0, OP_PUT_HOUR, 12},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 0, 30, 0, 0, OP_PUT_HOUR, 1},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 0, 30, 0, 0, OP_PUT_HOUR, 0},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 0, 30, 0, 0, OP_PUT_HOUR, 13},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_PUT_HOUR, 12},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_PUT_HOUR, 5},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_PUT_PERIOD, 1},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 2, 30, 0, 0, OP_PUT_PERIOD, 2},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 2, 30, 0, 0, OP_PUT_PERIOD, 3},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 2, 30, 0, 0, OP_PUT_PERIOD, 0},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_ADD_PERIODS, 1},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_ADD_PERIODS, 3},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_ADD_PERIODS, -1},
        {L"12HourClock", L"UTC", 0, 2024, 2, 29, 14, 30, 0, 0, OP_ADD_HOURS, 12},
        /* adds */
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_ADD_MONTHS, 1},
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_ADD_MONTHS, 13},
        {L"24HourClock", L"UTC", 0, 2024, 1, 31, 10, 0, 0, 0, OP_ADD_MONTHS, -2},
        {L"24HourClock", L"UTC", 0, 2024, 3, 31, 10, 0, 0, 0, OP_ADD_MONTHS, -1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_YEARS, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_YEARS, 4},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_YEARS, -2030},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_YEARS, 8000},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_ERAS, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_ERAS, 0},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_WEEKS, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_DAYS, 1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_DAYS, -365},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_HOURS, 14},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_MINUTES, -601},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_SECONDS, 86400},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_NANOSECONDS, 150},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_NANOSECONDS, -1},
        {L"24HourClock", L"UTC", 0, 2024, 2, 29, 10, 0, 0, 0, OP_ADD_NANOSECONDS, 2000000000},
        /* DST in Berlin: 2024-03-31 01:00 UTC is 03:00 CEST, 2024-10-27 01:00 UTC is 02:00 CET */
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 3, 31, 0, 30, 0, 0, OP_ADD_HOURS, 1},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 3, 31, 0, 30, 0, 0, OP_ADD_MINUTES, 60},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 3, 30, 1, 30, 0, 0, OP_ADD_DAYS, 1},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 3, 31, 0, 30, 0, 0, OP_PUT_HOUR, 2},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 3, 31, 0, 30, 0, 0, OP_PUT_HOUR, 3},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 10, 27, 0, 30, 0, 0, OP_ADD_HOURS, 1},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 10, 27, 0, 30, 0, 0, OP_PUT_HOUR, 2},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 10, 27, 1, 30, 0, 0, OP_PUT_HOUR, 2},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 10, 26, 0, 30, 0, 0, OP_ADD_DAYS, 1},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 10, 27, 0, 30, 0, 0, OP_PUT_MINUTE, 45},
        {L"24HourClock", L"Europe/Berlin", 0, 2024, 10, 27, 1, 30, 0, 0, OP_PUT_MINUTE, 45},
    };
    char tag[128];
    unsigned int i;
    ICalendar *cal;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        struct op_test *t = &tests[i];
        cal = create_calendar( L"en-US", L"GregorianCalendar", t->clock, t->tz, &hr );
        if (!cal) { trace( "P|op%u|create|hr=%#lx\n", i, hr ); continue; }
        set_datetime( cal, make_datetime( t->y, t->mo, t->d, t->h, t->mi, t->s, t->ticks ) );
        sprintf( tag, "op%u:%s:%s:%04d-%02d-%02dT%02d:%02d:%02dZ:%s(%d)", i, wine_dbgstr_w( t->clock ),
                 wine_dbgstr_w( t->tz ), t->y, t->mo, t->d, t->h, t->mi, t->s, op_names[t->op], t->value );
        dump_fields( cal, wine_dbg_sprintf( "%s:before", tag ) );
        hr = apply_op( cal, t->op, t->value );
        trace( "P|%s|hr|%#lx\n", tag, hr );
        dump_fields( cal, wine_dbg_sprintf( "%s:after", tag ) );
        ICalendar_Release( cal );
    }
}

static void probe_basics(void)
{
    IActivationFactory *factory = get_factory();
    ICalendarFactory2 *factory2 = NULL;
    ICalendarFactory *factory1 = NULL;
    IInspectable *inspectable = NULL;
    ICalendar *cal = NULL, *cal2 = NULL, *clone = NULL;
    TrustLevel level = 42;
    DateTime dt, before, after;
    INT32 cmp;
    ULONG count = 0;
    IID *iids = NULL;
    HSTRING s;
    HRESULT hr;

    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory, (void **)&factory1 );
    trace( "P|factory|QI(ICalendarFactory)|%#lx\n", hr );
    hr = IActivationFactory_QueryInterface( factory, &IID_ICalendarFactory2, (void **)&factory2 );
    trace( "P|factory|QI(ICalendarFactory2)|%#lx\n", hr );
    s = NULL; hr = IActivationFactory_GetRuntimeClassName( factory, &s );
    trace( "P|factory|GetRuntimeClassName|%s\n", dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
    hr = IActivationFactory_GetTrustLevel( factory, &level );
    trace( "P|factory|GetTrustLevel|hr=%#lx level=%d\n", hr, level );

    {
        FILETIME ft;
        GetSystemTimeAsFileTime( &ft );
        before.UniversalTime = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    trace( "P|default|ActivateInstance|%#lx\n", hr );
    hr = IInspectable_QueryInterface( inspectable, &IID_ICalendar, (void **)&cal );
    trace( "P|default|QI(ICalendar)|%#lx\n", hr );
    {
        FILETIME ft;
        GetSystemTimeAsFileTime( &ft );
        after.UniversalTime = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    hr = ICalendar_GetDateTime( cal, &dt );
    trace( "P|default|DateTime-in-activation-window|%d\n",
           dt.UniversalTime >= before.UniversalTime && dt.UniversalTime <= after.UniversalTime );
    trace( "P|default|DateTime-vs-before-ticks|%I64d\n", dt.UniversalTime - before.UniversalTime );
    s = NULL; hr = IInspectable_GetRuntimeClassName( inspectable, &s );
    trace( "P|default|GetRuntimeClassName|%s\n", dbg_hr_hs( hr, s ) ); WindowsDeleteString( s );
    hr = IInspectable_GetTrustLevel( inspectable, &level );
    trace( "P|default|GetTrustLevel|hr=%#lx level=%d\n", hr, level );
    hr = IInspectable_GetIids( inspectable, &count, &iids );
    trace( "P|default|GetIids|hr=%#lx count=%lu\n", hr, count );
    if (SUCCEEDED(hr))
    {
        ULONG i;
        for (i = 0; i < count; i++) trace( "P|default|GetIids[%lu]|%s\n", i, debugstr_guid( &iids[i] ) );
        CoTaskMemFree( iids );
    }
    {
        IUnknown *unk;
        hr = IInspectable_QueryInterface( inspectable, &IID_IAgileObject, (void **)&unk );
        trace( "P|default|QI(IAgileObject)|%#lx\n", hr ); if (SUCCEEDED(hr)) IUnknown_Release( unk );
        hr = IInspectable_QueryInterface( inspectable, &IID_ITimeZoneOnCalendar, (void **)&unk );
        trace( "P|default|QI(ITimeZoneOnCalendar)|%#lx\n", hr ); if (SUCCEEDED(hr)) IUnknown_Release( unk );
        hr = IInspectable_QueryInterface( inspectable, &IID_IMarshal, (void **)&unk );
        trace( "P|default|QI(IMarshal)|%#lx\n", hr ); if (SUCCEEDED(hr)) IUnknown_Release( unk );
    }
    dump_all( cal, "default" );

    /* SetToMin / SetToMax on the default calendar and on a UTC one */
    hr = ICalendar_SetToMin( cal );
    trace( "P|default|SetToMin|%#lx\n", hr );
    dump_fields( cal, "default:min" );
    hr = ICalendar_SetToMax( cal );
    trace( "P|default|SetToMax|%#lx\n", hr );
    dump_fields( cal, "default:max" );
    hr = ICalendar_SetToNow( cal );
    trace( "P|default|SetToNow|%#lx\n", hr );

    cal2 = create_calendar( L"en-US", L"GregorianCalendar", L"24HourClock", L"UTC", &hr );
    trace( "P|utc|create|%#lx\n", hr );
    if (cal2)
    {
        hr = ICalendar_SetToMin( cal2 );
        dump_fields( cal2, "utc:min" );
        hr = ICalendar_SetToMax( cal2 );
        dump_fields( cal2, "utc:max" );
        dump_all( cal2, "utc:max:all" );
        dt.UniversalTime = 0;
        hr = ICalendar_SetDateTime( cal2, dt );
        trace( "P|utc|SetDateTime(0)|%#lx\n", hr );
        dump_fields( cal2, "utc:zero" );
        dt.UniversalTime = -1;
        hr = ICalendar_SetDateTime( cal2, dt );
        trace( "P|utc|SetDateTime(-1)|%#lx\n", hr );
        dump_fields( cal2, "utc:minus1" );
        dt.UniversalTime = make_datetime( 1, 1, 1, 0, 0, 0, 0 );
        trace( "P|utc|make_datetime(0001)|%I64d\n", dt.UniversalTime );
        dt.UniversalTime = -504911232000000000LL;
        hr = ICalendar_SetDateTime( cal2, dt );
        trace( "P|utc|SetDateTime(0001-01-01)|%#lx\n", hr );
        dump_fields( cal2, "utc:year1" );
        dt.UniversalTime = -504911232000000000LL - 1;
        hr = ICalendar_SetDateTime( cal2, dt );
        trace( "P|utc|SetDateTime(0001-01-01 - 1)|%#lx\n", hr );
        dump_fields( cal2, "utc:before-year1" );
        dt.UniversalTime = 0x7fffffffffffffffLL;
        hr = ICalendar_SetDateTime( cal2, dt );
        trace( "P|utc|SetDateTime(INT64_MAX)|%#lx\n", hr );
        dump_fields( cal2, "utc:int64max" );

        /* Compare, CompareDateTime, CopyTo, Clone */
        set_datetime( cal2, make_datetime( 2024, 2, 29, 10, 0, 0, 0 ) );
        set_datetime( cal, make_datetime( 2024, 2, 29, 10, 0, 0, 1 ) );
        cmp = 42; hr = ICalendar_Compare( cal2, cal, &cmp );
        trace( "P|compare|earlier-vs-later|hr=%#lx %d\n", hr, cmp );
        cmp = 42; hr = ICalendar_Compare( cal, cal2, &cmp );
        trace( "P|compare|later-vs-earlier|hr=%#lx %d\n", hr, cmp );
        cmp = 42; hr = ICalendar_Compare( cal, cal, &cmp );
        trace( "P|compare|self|hr=%#lx %d\n", hr, cmp );
        cmp = 42; hr = ICalendar_Compare( cal, NULL, &cmp );
        trace( "P|compare|null|hr=%#lx %d\n", hr, cmp );
        dt.UniversalTime = make_datetime( 2025, 1, 1, 0, 0, 0, 0 );
        cmp = 42; hr = ICalendar_CompareDateTime( cal, dt, &cmp );
        trace( "P|compare|datetime-later|hr=%#lx %d\n", hr, cmp );
        dt.UniversalTime = make_datetime( 2000, 1, 1, 0, 0, 0, 0 );
        cmp = 42; hr = ICalendar_CompareDateTime( cal, dt, &cmp );
        trace( "P|compare|datetime-earlier|hr=%#lx %d\n", hr, cmp );

        hr = ICalendar_CopyTo( cal2, cal );
        trace( "P|copyto|hr|%#lx\n", hr );
        dump_all( cal, "copyto:target" );
        hr = ICalendar_CopyTo( cal2, NULL );
        trace( "P|copyto|null|%#lx\n", hr );

        hr = ICalendar_Clone( cal2, &clone );
        trace( "P|clone|hr|%#lx same=%d\n", hr, clone == cal2 );
        if (clone) { dump_all( clone, "clone" ); ICalendar_Release( clone ); }

        /* calendar systems, clocks, numeral systems */
        {
            static const WCHAR *systems[] = {L"GregorianCalendar", L"JapaneseCalendar", L"HebrewCalendar",
                L"HijriCalendar", L"UmAlQuraCalendar", L"TaiwanCalendar", L"KoreanCalendar", L"ThaiCalendar",
                L"JulianCalendar", L"PersianCalendar", L"gregoriancalendar", L"Foo", L""};
            static const WCHAR *clocks[] = {L"12HourClock", L"24HourClock", L"24hourclock", L"Foo", L""};
            static const WCHAR *numerals[] = {L"Latn", L"Arab", L"latn", L"Foo", L""};
            unsigned int i;
            for (i = 0; i < ARRAY_SIZE(systems); i++)
            {
                HSTRING str = hs( systems[i] );
                set_datetime( cal2, make_datetime( 2024, 2, 29, 10, 0, 0, 0 ) );
                hr = ICalendar_ChangeCalendarSystem( cal2, str );
                trace( "P|system|%s|hr=%#lx\n", wine_dbgstr_w( systems[i] ), hr );
                if (SUCCEEDED(hr))
                {
                    char t[64];
                    sprintf( t, "system:%s", wine_dbgstr_w( systems[i] ) );
                    { ICalendar *c = cal2; const char *tag = t; (void)c;
                      HSTRING v = NULL; HRESULT h = ICalendar_GetCalendarSystem( cal2, &v );
                      trace( "P|%s|GetCalendarSystem|%s\n", tag, dbg_hr_hs( h, v ) ); WindowsDeleteString( v ); }
                    dump_fields( cal2, t );
                }
                WindowsDeleteString( str );
            }
            ICalendar_ChangeCalendarSystem( cal2, (s = hs( L"GregorianCalendar" )) ); WindowsDeleteString( s );
            for (i = 0; i < ARRAY_SIZE(clocks); i++)
            {
                HSTRING str = hs( clocks[i] ), v = NULL;
                hr = ICalendar_ChangeClock( cal2, str );
                ICalendar_GetClock( cal2, &v );
                trace( "P|clock|%s|hr=%#lx now=%s\n", wine_dbgstr_w( clocks[i] ), hr, dbg_hs( v ) );
                WindowsDeleteString( v ); WindowsDeleteString( str );
            }
            for (i = 0; i < ARRAY_SIZE(numerals); i++)
            {
                HSTRING str = hs( numerals[i] ), v = NULL, y = NULL;
                hr = ICalendar_put_NumeralSystem( cal2, str );
                ICalendar_get_NumeralSystem( cal2, &v );
                ICalendar_YearAsString( cal2, &y );
                trace( "P|numeral|%s|hr=%#lx now=%s year=%s\n", wine_dbgstr_w( numerals[i] ), hr, dbg_hs( v ),
                       dbg_hs( y ) );
                WindowsDeleteString( v ); WindowsDeleteString( y ); WindowsDeleteString( str );
            }
        }

        /* time zones */
        {
            static const WCHAR *zones[] = {L"UTC", L"Etc/UTC", L"Etc/GMT", L"GMT", L"Europe/Berlin",
                L"europe/berlin", L"W. Europe Standard Time", L"America/New_York", L"Asia/Kolkata",
                L"Australia/Lord_Howe", L"Invalid/Zone", L""};
            ITimeZoneOnCalendar *tz = NULL;
            unsigned int i;
            ICalendar_QueryInterface( cal2, &IID_ITimeZoneOnCalendar, (void **)&tz );
            for (i = 0; tz && i < ARRAY_SIZE(zones); i++)
            {
                HSTRING str = hs( zones[i] ), v = NULL, f = NULL, a = NULL;
                INT32 hour = -1;
                set_datetime( cal2, make_datetime( 2024, 7, 1, 12, 0, 0, 0 ) );
                hr = ITimeZoneOnCalendar_ChangeTimeZone( tz, str );
                ITimeZoneOnCalendar_GetTimeZone( tz, &v );
                ITimeZoneOnCalendar_TimeZoneAsFullString( tz, &f );
                ITimeZoneOnCalendar_TimeZoneAsString( tz, 3, &a );
                ICalendar_get_Hour( cal2, &hour );
                trace( "P|tz|%s|hr=%#lx now=%s full=%s short=%s hour@12Z=%d\n", wine_dbgstr_w( zones[i] ), hr,
                       dbg_hs( v ), dbg_hs( f ), dbg_hs( a ), hour );
                WindowsDeleteString( v ); WindowsDeleteString( f ); WindowsDeleteString( a );
                WindowsDeleteString( str );
            }
            if (tz) ITimeZoneOnCalendar_Release( tz );
            for (i = 0; i < ARRAY_SIZE(zones); i++)
            {
                ICalendar *c = create_calendar( L"en-US", L"GregorianCalendar", L"24HourClock", zones[i], &hr );
                trace( "P|tzcreate|%s|hr=%#lx\n", wine_dbgstr_w( zones[i] ), hr );
                if (c) ICalendar_Release( c );
            }
        }
        ICalendar_Release( cal2 );
    }

    /* factory argument validation */
    {
        static const WCHAR *langs[] = {L"en-US", L"de-DE", L"xx-invalid", L"de", L"ja-JP", L"ar-SA", L"", L"en"};
        unsigned int i;
        for (i = 0; i < ARRAY_SIZE(langs); i++)
        {
            ICalendar *c = create_calendar( langs[i], NULL, NULL, NULL, &hr );
            trace( "P|lang|%s|hr=%#lx\n", wine_dbgstr_w( langs[i] ), hr );
            if (c)
            {
                char t[64];
                sprintf( t, "lang:%s", wine_dbgstr_w( langs[i] ) );
                set_datetime( c, make_datetime( 2024, 2, 29, 13, 45, 30, 1234567 ) );
                dump_all( c, t );
                ICalendar_Release( c );
            }
        }
        {
            IIterable_HSTRING *two = create_languages( (const WCHAR *[]){L"xx-invalid", L"de-DE"}, 2 );
            ICalendar *c = NULL;
            hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, two, &c );
            trace( "P|lang|xx-invalid+de-DE|hr=%#lx\n", hr );
            if (c) { dump_all( c, "lang:xx-invalid+de-DE" ); ICalendar_Release( c ); }
            IIterable_HSTRING_Release( two );
        }
        {
            IIterable_HSTRING *none = create_languages( NULL, 0 );
            ICalendar *c = NULL;
            hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, none, &c );
            trace( "P|lang|empty-list|hr=%#lx\n", hr );
            if (c) { dump_all( c, "lang:empty-list" ); ICalendar_Release( c ); }
            IIterable_HSTRING_Release( none );
            c = NULL;
            hr = ICalendarFactory_CreateCalendarDefaultCalendarAndClock( factory1, NULL, &c );
            trace( "P|lang|null|hr=%#lx\n", hr );
            if (c) ICalendar_Release( c );
        }
        {
            ICalendar *c = create_calendar( L"en-US", L"Foo", L"24HourClock", NULL, &hr );
            trace( "P|create|bad-system|hr=%#lx\n", hr ); if (c) ICalendar_Release( c );
            c = create_calendar( L"en-US", L"GregorianCalendar", L"Foo", NULL, &hr );
            trace( "P|create|bad-clock|hr=%#lx\n", hr ); if (c) ICalendar_Release( c );
            c = create_calendar( L"en-US", L"JapaneseCalendar", L"12HourClock", NULL, &hr );
            trace( "P|create|japanese|hr=%#lx\n", hr ); if (c) ICalendar_Release( c );
        }
    }

    /* full dumps at a fixed instant for several language/clock/zone combinations */
    {
        static const struct { const WCHAR *lang, *clock, *tz; } combos[] =
        {
            {L"en-US", L"24HourClock", L"UTC"},
            {L"en-US", L"12HourClock", L"UTC"},
            {L"de-DE", L"24HourClock", L"Europe/Berlin"},
            {L"de-DE", L"12HourClock", L"Europe/Berlin"},
            {L"en-US", L"12HourClock", L"America/New_York"},
            {L"ja-JP", L"24HourClock", L"UTC"},
        };
        static const struct { int y, mo, d, h, mi, s, ticks; } instants[] =
        {
            {2024, 2, 29, 13, 45, 30, 1234567},
            {2024, 7, 1, 0, 5, 7, 0},
            {1999, 12, 31, 23, 59, 59, 9999999},
            {2003, 5, 4, 12, 0, 0, 100},
        };
        unsigned int i, j;
        for (i = 0; i < ARRAY_SIZE(combos); i++)
        {
            ICalendar *c = create_calendar( combos[i].lang, L"GregorianCalendar", combos[i].clock, combos[i].tz, &hr );
            if (!c) { trace( "P|combo%u|create|hr=%#lx\n", i, hr ); continue; }
            for (j = 0; j < ARRAY_SIZE(instants); j++)
            {
                char t[128];
                sprintf( t, "combo:%s:%s:%s:%04d-%02d-%02dT%02d:%02d:%02d.%07dZ", wine_dbgstr_w( combos[i].lang ),
                         wine_dbgstr_w( combos[i].clock ), wine_dbgstr_w( combos[i].tz ), instants[j].y,
                         instants[j].mo, instants[j].d, instants[j].h, instants[j].mi, instants[j].s,
                         instants[j].ticks );
                set_datetime( c, make_datetime( instants[j].y, instants[j].mo, instants[j].d, instants[j].h,
                                                instants[j].mi, instants[j].s, instants[j].ticks ) );
                dump_all( c, t );
            }
            ICalendar_Release( c );
        }
    }

    ICalendar_Release( cal );
    IInspectable_Release( inspectable );
    if (factory1) ICalendarFactory_Release( factory1 );
    if (factory2) ICalendarFactory2_Release( factory2 );
    IActivationFactory_Release( factory );
}

START_TEST(calendar)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    {
        WCHAR name[LOCALE_NAME_MAX_LENGTH];
        TIME_ZONE_INFORMATION tzi;
        GetUserDefaultLocaleName( name, ARRAY_SIZE(name) );
        trace( "P|env|UserDefaultLocaleName|%s\n", wine_dbgstr_w( name ) );
        GetTimeZoneInformation( &tzi );
        trace( "P|env|TimeZone|%s bias=%ld\n", wine_dbgstr_w( tzi.StandardName ), tzi.Bias );
        trace( "P|env|UserDefaultUILanguage|%#x\n", GetUserDefaultUILanguage() );
    }

    probe_basics();
    probe_ops();

    RoUninitialize();
}
