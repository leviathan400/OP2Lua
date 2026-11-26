' PlacementText.vb
'
' Reading and rewriting placement.lua as text, with no UI: the editor edits a file a person (or op2dll) wrote, so it
' changes only the tables it owns (units, regions) and leaves the rest - comments, formatting, fields it doesn't
' know - as they were. Also the mission type names and finding a DLL export (MissionDll.vb builds the mission DLL).
'
' Lua comments and strings are masked out (MaskLua) before any key or brace is matched, so a brace in a comment or a
' name doesn't throw the table bounds off.

Imports System.Collections.Generic
Imports System.Text
Imports System.Text.RegularExpressions

Public Module PlacementText

    ' ===== masking =====

    ' The text with every comment character blanked to a space and every character inside a string to "_" (the
    ' quotes and newlines stay), the same length as the text, so an index into one is an index into the other.
    Public Function MaskLua(text As String) As String
        Dim sb As New StringBuilder(text)
        Dim i As Integer = 0
        Dim n As Integer = text.Length
        While i < n
            Dim c As Char = text(i)
            If c = "-"c AndAlso i + 1 < n AndAlso text(i + 1) = "-"c Then
                Dim level As Integer = LongBracketLevel(text, i + 2)
                If level >= 0 Then
                    ' --[[ ... ]] (or --[==[ ... ]==])
                    Dim close As String = "]" & New String("="c, level) & "]"
                    Dim e As Integer = text.IndexOf(close, i + 2 + level + 2, StringComparison.Ordinal)
                    Dim stopAt As Integer = If(e < 0, n, e + close.Length)
                    BlankRange(sb, i, stopAt, " "c)
                    i = stopAt
                Else
                    Dim e As Integer = i
                    While e < n AndAlso text(e) <> ControlChars.Lf AndAlso text(e) <> ControlChars.Cr
                        e += 1
                    End While
                    BlankRange(sb, i, e, " "c)
                    i = e
                End If
            ElseIf c = """"c OrElse c = "'"c Then
                Dim e As Integer = i + 1
                While e < n AndAlso text(e) <> c AndAlso text(e) <> ControlChars.Lf
                    If text(e) = "\"c Then e += 1
                    e += 1
                End While
                BlankRange(sb, i + 1, Math.Min(e, n), "_"c)
                i = e + 1
            ElseIf c = "["c AndAlso LongBracketLevel(text, i) >= 0 Then
                Dim level As Integer = LongBracketLevel(text, i)
                Dim close As String = "]" & New String("="c, level) & "]"
                Dim e As Integer = text.IndexOf(close, i + level + 2, StringComparison.Ordinal)
                Dim stopAt As Integer = If(e < 0, n, e)
                BlankRange(sb, i + level + 2, stopAt, "_"c)
                i = If(e < 0, n, e + close.Length)
            Else
                i += 1
            End If
        End While
        Return sb.ToString()
    End Function

    ' The level of a Lua long bracket "[", "="*, "[" starting at i (the count of "="), or -1 if there is none.
    Private Function LongBracketLevel(text As String, i As Integer) As Integer
        If i >= text.Length OrElse text(i) <> "["c Then Return -1
        Dim j As Integer = i + 1
        While j < text.Length AndAlso text(j) = "="c
            j += 1
        End While
        If j < text.Length AndAlso text(j) = "["c Then Return j - i - 1
        Return -1
    End Function

    ' Overwrite sb[from, upTo) with fill, leaving line breaks so line numbers still match.
    Private Sub BlankRange(sb As StringBuilder, from As Integer, upTo As Integer, fill As Char)
        For k As Integer = from To upTo - 1
            If sb(k) <> ControlChars.Lf AndAlso sb(k) <> ControlChars.Cr Then sb(k) = fill
        Next
    End Sub

    ' Index of the "}" matching the "{" at openIndex in masked text (-1 if unbalanced).
    Public Function MatchBrace(masked As String, openIndex As Integer) As Integer
        Dim depth As Integer = 0
        For i As Integer = openIndex To masked.Length - 1
            Dim c As Char = masked(i)
            If c = "{"c Then
                depth += 1
            ElseIf c = "}"c Then
                depth -= 1
                If depth = 0 Then Return i
            End If
        Next
        Return -1
    End Function

    ' The first   key = { ... }   table outside comments and strings: where the key starts, its "{" and "}".
    Public Function FindTable(masked As String, key As String, ByRef keyStart As Integer, ByRef openIdx As Integer,
                              ByRef closeIdx As Integer) As Boolean
        Dim m As Match = Regex.Match(masked, "\b" & Regex.Escape(key) & "\s*=\s*\{")
        If Not m.Success Then Return False
        keyStart = m.Index
        openIdx = m.Index + m.Length - 1
        closeIdx = MatchBrace(masked, openIdx)
        Return closeIdx > openIdx
    End Function

    ' Replace the   key = { ... }   table with newBlock, or add it before the file's last "}" if there is none.
    Public Function ReplaceTable(text As String, key As String, newBlock As String) As String
        Dim masked As String = MaskLua(text)
        Dim ks, op, cl As Integer
        If FindTable(masked, key, ks, op, cl) Then
            Return text.Substring(0, ks) & newBlock & text.Substring(cl + 1)
        End If
        Dim lastBrace As Integer = masked.LastIndexOf("}"c)
        Dim nl As String = NewLineOf(text)
        If lastBrace < 0 Then Return text & nl & newBlock & nl
        Return text.Substring(0, lastBrace) & "  " & newBlock & "," & nl & text.Substring(lastBrace)
    End Function

    ' One table entry inside a table: its "{" and "}" (in the text's indices), the text before it since the previous
    ' entry (comments, separators), and its key if it is written   name = { ... }.
    Public Structure TableEntry
        Public Open As Integer
        Public Close As Integer
        Public Lead As String
        Public Key As String
    End Structure

    ' The table entries at the top level of the table whose braces are openIdx..closeIdx; tail is the text after the
    ' last one, up to the closing brace.
    Public Function Entries(text As String, masked As String, openIdx As Integer, closeIdx As Integer,
                            ByRef tail As String) As List(Of TableEntry)
        Dim result As New List(Of TableEntry)()
        Dim prevEnd As Integer = openIdx
        Dim i As Integer = openIdx + 1
        While i < closeIdx
            If masked(i) = "{"c Then
                Dim e As Integer = MatchBrace(masked, i)
                If e < 0 OrElse e > closeIdx Then Exit While
                Dim lead As String = text.Substring(prevEnd + 1, i - prevEnd - 1)
                Dim km As Match = Regex.Match(MaskLua(lead), "([A-Za-z_]\w*)\s*=\s*$")
                result.Add(New TableEntry With {.Open = i, .Close = e, .Lead = lead, .Key = If(km.Success, km.Groups(1).Value, Nothing)})
                prevEnd = e
                i = e + 1
            Else
                i += 1
            End If
        End While
        tail = text.Substring(prevEnd + 1, closeIdx - prevEnd - 1)
        Return result
    End Function

    ' ===== fields inside one entry =====

    ' Where a field's value is in an entry's text: valueStart..valueEnd (exclusive), and where the key starts.
    Private Function FindField(raw As String, masked As String, key As String, ByRef keyStart As Integer,
                               ByRef valueStart As Integer, ByRef valueEnd As Integer) As Boolean
        ' The field must sit at the entry's top level, not inside a nested table such as at = { x = .. }.
        For Each m As Match In Regex.Matches(masked, "\b" & Regex.Escape(key) & "\s*=\s*")
            If Depth(masked, m.Index) <> 0 Then Continue For
            keyStart = m.Index
            valueStart = m.Index + m.Length
            If valueStart >= raw.Length Then Return False
            Dim c As Char = masked(valueStart)
            If c = """"c OrElse c = "'"c Then
                Dim e As Integer = masked.IndexOf(c, valueStart + 1)
                If e < 0 Then Return False
                valueEnd = e + 1
            ElseIf c = "{"c Then
                Dim e As Integer = MatchBrace(masked, valueStart)
                If e < 0 Then Return False
                valueEnd = e + 1
            Else
                Dim e As Integer = valueStart
                While e < masked.Length AndAlso masked(e) <> ","c AndAlso masked(e) <> "}"c AndAlso masked(e) <> ";"c AndAlso
                      Not Char.IsWhiteSpace(masked(e))
                    e += 1
                End While
                valueEnd = e
            End If
            Return True
        Next
        Return False
    End Function

    ' Brace depth at index i of a masked entry body.
    Private Function Depth(masked As String, i As Integer) As Integer
        Dim d As Integer = 0
        For k As Integer = 0 To i - 1
            If masked(k) = "{"c Then d += 1 Else If masked(k) = "}"c Then d -= 1
        Next
        Return d
    End Function

    ' A string field's value (Nothing when the field is absent or isn't a string).
    Public Function StringField(raw As String, key As String) As String
        Dim masked As String = MaskLua(raw)
        Dim ks, vs, ve As Integer
        If Not FindField(raw, masked, key, ks, vs, ve) Then Return Nothing
        If raw(vs) <> """"c AndAlso raw(vs) <> "'"c Then Return Nothing
        Return raw.Substring(vs + 1, ve - vs - 2)
    End Function

    ' An integer field's value, or fallback.
    Public Function IntField(raw As String, key As String, fallback As Integer) As Integer
        Dim masked As String = MaskLua(raw)
        Dim ks, vs, ve As Integer
        If Not FindField(raw, masked, key, ks, vs, ve) Then Return fallback
        Dim v As Integer
        If Integer.TryParse(raw.Substring(vs, ve - vs), v) Then Return v
        Return fallback
    End Function

    ' An   at = { x, y }   location; False for any other form (a raw location { x =, y =, raw = true }, or none).
    Public Function AtField(raw As String, ByRef x As Integer, ByRef y As Integer) As Boolean
        Dim masked As String = MaskLua(raw)
        Dim ks, vs, ve As Integer
        If Not FindField(raw, masked, "at", ks, vs, ve) Then Return False
        Dim m As Match = Regex.Match(raw.Substring(vs, ve - vs), "^\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}$")
        If Not m.Success Then Return False
        x = Integer.Parse(m.Groups(1).Value)
        y = Integer.Parse(m.Groups(2).Value)
        Return True
    End Function

    ' Set a field to a Lua value (written as given), adding it at the end of the entry if it is absent.
    Public Function SetField(raw As String, key As String, luaValue As String) As String
        Dim masked As String = MaskLua(raw)
        Dim ks, vs, ve As Integer
        If FindField(raw, masked, key, ks, vs, ve) Then
            Return raw.Substring(0, vs) & luaValue & raw.Substring(ve)
        End If
        Dim body As String = raw.TrimEnd()
        Dim trailing As String = raw.Substring(body.Length)
        Dim sep As String = If(body.Length = 0 OrElse body.EndsWith(",") OrElse body.EndsWith(";"), " ", ", ")
        If body.Length = 0 Then sep = " "
        Return body & sep & key & " = " & luaValue & If(trailing.Length = 0, " ", trailing)
    End Function

    ' Remove a field (and the comma that went with it).
    Public Function RemoveField(raw As String, key As String) As String
        Dim masked As String = MaskLua(raw)
        Dim ks, vs, ve As Integer
        If Not FindField(raw, masked, key, ks, vs, ve) Then Return raw
        ' Prefer the comma before the field; else take the one after it.
        Dim b As Integer = ks - 1
        While b >= 0 AndAlso Char.IsWhiteSpace(masked(b))
            b -= 1
        End While
        If b >= 0 AndAlso masked(b) = ","c Then Return raw.Substring(0, b) & raw.Substring(ve)
        Dim a As Integer = ve
        While a < masked.Length AndAlso Char.IsWhiteSpace(masked(a))
            a += 1
        End While
        If a < masked.Length AndAlso masked(a) = ","c Then
            a += 1
            While a < masked.Length AndAlso masked(a) = " "c
                a += 1
            End While
        End If
        Return raw.Substring(0, ks) & raw.Substring(a)
    End Function

    ' The text's line ending: LF if it has no CRLF (op2dll writes LF), else CRLF.
    Public Function NewLineOf(text As String) As String
        Return If(text.Contains(vbCrLf) OrElse Not text.Contains(vbLf), vbCrLf, vbLf)
    End Function

    ' s as a Lua string literal (quotes and backslashes escaped).
    Public Function LuaString(s As String) As String
        Return """" & s.Replace("\", "\\").Replace("""", "\""") & """"
    End Function

    ' ===== units =====

    ' The units table as the editor holds it: its entries, the header text up to its "{" and the text after the last
    ' entry. Found is False when the file has no units table.
    Public Class UnitsBlock
        Public Units As New List(Of MissionUnit)
        Public Header As String = "units = {"
        Public Tail As String = Nothing      ' Nothing: no table in the file yet
        Public NewLine As String = vbCrLf    ' the file's line ending, for the lines the editor adds
        Public Found As Boolean
        Public OrigCount As Integer          ' entries in the file's table when it was read
    End Class

    ' The file's units table: each entry's text kept as it is (Raw, Lead), with the fields the editor shows read out.
    Public Function ParseUnits(text As String) As UnitsBlock
        Dim result As New UnitsBlock()
        result.NewLine = NewLineOf(text)
        Dim masked As String = MaskLua(text)
        Dim ks, op, cl As Integer
        If Not FindTable(masked, "units", ks, op, cl) Then Return result
        result.Found = True
        result.Header = text.Substring(ks, op - ks + 1)
        Dim tail As String = Nothing
        Dim index As Integer = 0
        For Each en As TableEntry In Entries(text, masked, op, cl, tail)
            Dim raw As String = text.Substring(en.Open + 1, en.Close - en.Open - 1)
            Dim u As New MissionUnit()
            u.Raw = raw
            u.Lead = en.Lead
            u.OrigIndex = index
            index += 1
            u.HasAt = AtField(raw, u.X, u.Y)
            u.UnitType = If(StringField(raw, "type"), "Unit")
            u.Player = IntField(raw, "player", 1)
            u.Name = StringField(raw, "name")
            u.Weapon = If(StringField(raw, "weapon"), "")
            u.OrigType = u.UnitType : u.OrigPlayer = u.Player : u.OrigX = u.X : u.OrigY = u.Y : u.OrigWeapon = u.Weapon
            result.Units.Add(u)
        Next
        result.Tail = tail
        result.OrigCount = index
        Return result
    End Function

    ' One unit's text between its braces: a unit placed in the editor is written out; a loaded one keeps its own
    ' text, with only the fields the editor changed rewritten.
    Public Function UnitEntryText(u As MissionUnit) As String
        If u.Raw Is Nothing Then
            Dim s As String = " type = " & LuaString(u.UnitType) & ", player = " & u.Player & ", at = { " & u.X & ", " & u.Y & " }"
            If Not String.IsNullOrEmpty(u.Weapon) Then s &= ", weapon = " & LuaString(u.Weapon)
            Return s & " "
        End If
        Dim raw As String = u.Raw
        If u.UnitType <> u.OrigType Then raw = SetField(raw, "type", LuaString(u.UnitType))
        If u.Player <> u.OrigPlayer Then raw = SetField(raw, "player", u.Player.ToString())
        If u.HasAt AndAlso (u.X <> u.OrigX OrElse u.Y <> u.OrigY) Then raw = SetField(raw, "at", "{ " & u.X & ", " & u.Y & " }")
        If u.Weapon <> u.OrigWeapon Then
            raw = If(String.IsNullOrEmpty(u.Weapon), RemoveField(raw, "weapon"), SetField(raw, "weapon", LuaString(u.Weapon)))
        End If
        Return raw
    End Function

    ' The units table text for block. A file's units table read and written back unchanged is the same text.
    Public Function EmitUnits(block As UnitsBlock) As String
        Dim sb As New StringBuilder(block.Header)
        For i As Integer = 0 To block.Units.Count - 1
            Dim u As MissionUnit = block.Units(i)
            Dim lead As String = If(u.Lead, block.NewLine & "    ")
            Dim hasComma As Boolean = MaskLua(lead).Contains(",")
            If i = 0 AndAlso hasComma Then
                lead = RemoveFirstComma(lead)            ' the first entry now (the ones before it were deleted)
            ElseIf i > 0 AndAlso Not hasComma Then
                lead = "," & lead                       ' a new entry, or the old last one with an entry after it
            End If
            sb.Append(lead).Append("{").Append(UnitEntryText(u)).Append("}")
        Next
        Dim tail As String = If(block.Tail, block.NewLine & "  ")
        If block.Units.Count = 0 Then
            tail = RemoveFirstComma(tail)                ' no entry left for a trailing comma to follow
        ElseIf block.Units(block.Units.Count - 1).Raw Is Nothing Then
            ' The last entry is new: end it with a comma and put the closing brace on its own line.
            If Not tail.Contains(vbLf) Then tail = block.NewLine & "  "
            If Not MaskLua(tail).Contains(",") Then tail = "," & tail
        End If
        sb.Append(tail).Append("}")
        Return sb.ToString()
    End Function

    ' s without its first comma outside comments and strings (the separator before an entry).
    Private Function RemoveFirstComma(s As String) As String
        Dim k As Integer = MaskLua(s).IndexOf(","c)
        If k < 0 Then Return s
        Return s.Substring(0, k) & s.Substring(k + 1)
    End Function

    ' Remove unit i from block. The text between two entries is: the rest of the earlier entry's line (its separator
    ' and a comment on that line), then the lines before the next entry (a heading such as "-- Player 2's base").
    ' The deleted entry's own line comment goes with it; the line comment of the entry before it, and a heading
    ' above it, stay.
    Public Sub DeleteUnit(block As UnitsBlock, i As Integer)
        Dim gone As MissionUnit = block.Units(i)
        block.Units.RemoveAt(i)
        If gone.Lead Is Nothing Then Return                     ' placed in the editor: nothing around it to keep
        Dim following As String = If(i < block.Units.Count, block.Units(i).Lead, If(block.Tail, ""))
        If following Is Nothing Then Return                     ' the next one was placed in the editor too
        Dim goneRest As String = AfterFirstLine(gone.Lead)
        Dim rest As String = AfterFirstLine(following)
        If goneRest.Contains("--") Then rest = goneRest.TrimEnd(" "c, ControlChars.Tab) & rest
        Dim merged As String = FirstLine(gone.Lead) & rest
        If i < block.Units.Count Then block.Units(i).Lead = merged Else block.Tail = merged
    End Sub

    ' A lead up to and including its first line break (all of it if it has none), and what comes after that.
    Private Function FirstLine(s As String) As String
        Dim k As Integer = s.IndexOf(ControlChars.Lf)
        Return If(k < 0, s, s.Substring(0, k + 1))
    End Function

    Private Function AfterFirstLine(s As String) As String
        Dim k As Integer = s.IndexOf(ControlChars.Lf)
        Return If(k < 0, "", s.Substring(k + 1))
    End Function

    ' A beacon's   after = n   counts the units table's entries before it. With some of the loaded units deleted,
    ' n becomes the number of the remaining ones that were among those n, so each beacon still comes after the same
    ' unit. Units placed in the editor go at the end and don't change it.
    Public Function FixBeaconAfter(text As String, block As UnitsBlock) As String
        Dim masked As String = MaskLua(text)
        Dim ks, op, cl As Integer
        If Not FindTable(masked, "beacons", ks, op, cl) Then Return text
        Dim deleted As New List(Of Integer)()
        Dim kept As New HashSet(Of Integer)()
        For Each u As MissionUnit In block.Units
            If u.OrigIndex >= 0 Then kept.Add(u.OrigIndex)
        Next
        For k As Integer = 0 To block.OrigCount - 1
            If Not kept.Contains(k) Then deleted.Add(k)
        Next
        If deleted.Count = 0 Then Return text
        Dim sb As New StringBuilder()
        Dim last As Integer = 0
        For Each m As Match In New Regex("\bafter\s*=\s*(\d+)").Matches(masked, op)
            If m.Index > cl Then Exit For
            Dim n As Integer = Integer.Parse(m.Groups(1).Value)
            Dim n2 As Integer = n
            For Each k As Integer In deleted
                If k < n Then n2 -= 1
            Next
            sb.Append(text, last, m.Groups(1).Index - last).Append(n2)
            last = m.Groups(1).Index + m.Groups(1).Length
        Next
        sb.Append(text, last, text.Length - last)
        Return sb.ToString()
    End Function

    ' ===== beacons, walls and markers (read-only) =====

    Public Function ParseMapObjects(text As String) As List(Of MissionMapObject)
        Dim result As New List(Of MissionMapObject)()
        Dim masked As String = MaskLua(text)
        ' Each table, the kind it holds and the type an entry without one gets (as OP2Lua's Placement.cpp defaults it).
        Dim specs As Tuple(Of String, MapObjectKind, String)() = {
                Tuple.Create("beacons", MapObjectKind.Beacon, "MiningBeacon"),
                Tuple.Create("walls", MapObjectKind.Wall, "Wall"),
                Tuple.Create("markers", MapObjectKind.Marker, "Circle")}
        For Each spec As Tuple(Of String, MapObjectKind, String) In specs
            Dim ks, op, cl As Integer
            If Not FindTable(masked, spec.Item1, ks, op, cl) Then Continue For
            Dim tail As String = Nothing
            For Each en As TableEntry In Entries(text, masked, op, cl, tail)
                Dim raw As String = text.Substring(en.Open + 1, en.Close - en.Open - 1)
                Dim o As New MissionMapObject()
                If Not AtField(raw, o.X, o.Y) Then Continue For
                o.Kind = spec.Item2
                o.ObjType = If(StringField(raw, "type"), spec.Item3)
                o.Ore = If(StringField(raw, "ore"), "Random")
                o.Name = If(spec.Item2 = MapObjectKind.Marker, en.Key, StringField(raw, "name"))
                result.Add(o)
            Next
        Next
        Return result
    End Function

    ' ===== regions =====

    ' The regions table:   name = { x, y }   (a point) and   name = { x1, y1, x2, y2 }   (a rectangle).
    Public Function ParseRegions(text As String) As List(Of MissionRegion)
        Dim result As New List(Of MissionRegion)()
        Dim masked As String = MaskLua(text)
        Dim ks, op, cl As Integer
        If Not FindTable(masked, "regions", ks, op, cl) Then Return result
        Dim inner As String = masked.Substring(op + 1, cl - op - 1)
        Dim entry As New Regex("([A-Za-z_]\w*)\s*=\s*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*,\s*(-?\d+)\s*)?\}")
        For Each em As Match In entry.Matches(inner)
            Dim r As New MissionRegion()
            r.Name = em.Groups(1).Value
            r.X1 = Integer.Parse(em.Groups(2).Value)
            r.Y1 = Integer.Parse(em.Groups(3).Value)
            If em.Groups(4).Success Then
                r.Kind = RegionKind.Rectangle
                r.X2 = Integer.Parse(em.Groups(4).Value)
                r.Y2 = Integer.Parse(em.Groups(5).Value)
            Else
                r.Kind = RegionKind.Point
                r.X2 = r.X1 : r.Y2 = r.Y1
            End If
            result.Add(r)
        Next
        Return result
    End Function

    ' ===== mission types and DLL exports =====

    ' A placement type name -> the DescBlock's MissionType. Both the SDK's names (MultiLastOneStanding, as op2dll
    ' writes them) and op2lua-newmission's --type names (LastOneStanding) are accepted, and a campaign mission's
    ' number, 1 to 12, as op2dll writes it ("1"). 0 = not a mission type.
    Public Function MissionTypeValue(name As String) As Integer
        Dim campaign As Integer
        If Integer.TryParse(If(name, "").Trim(), campaign) AndAlso campaign >= 1 AndAlso campaign <= 12 Then Return campaign
        Select Case If(name, "").Trim().ToLowerInvariant()
            Case "colony" : Return -1
            Case "autodemo" : Return -2
            Case "tutorial" : Return -3
            Case "multilandrush", "landrush" : Return -4
            Case "multispacerace", "spacerace" : Return -5
            Case "multiresourcerace", "resourcerace" : Return -6
            Case "multimidas", "midas" : Return -7
            Case "multilastonestanding", "lastonestanding" : Return -8
            Case Else : Return 0
        End Select
    End Function

    ' Land Rush, Space Race, Resource Race, Midas or Last One Standing: listed in the multiplayer lobby.
    Public Function IsMultiplayerType(name As String) As Boolean
        Return MissionTypeValue(name) <= -4
    End Function

    ' The file offset of a named export's data in a PE image, or -1.
    Public Function ExportFileOffset(buf() As Byte, name As String) As Integer
        Try
            Dim pe As Integer = BitConverter.ToInt32(buf, &H3C)
            If BitConverter.ToUInt32(buf, pe) <> &H4550UI Then Return -1                 ' "PE\0\0"
            Dim numSections As Integer = BitConverter.ToUInt16(buf, pe + 6)
            Dim optSize As Integer = BitConverter.ToUInt16(buf, pe + 20)
            Dim opt As Integer = pe + 24
            Dim magic As Integer = BitConverter.ToUInt16(buf, opt)
            Dim dataDirs As Integer = opt + If(magic = &H20B, 112, 96)                   ' PE32+ or PE32
            Dim sections As Integer = opt + optSize
            Dim toOffset As Func(Of Integer, Integer) =
                Function(rva As Integer)
                    For s As Integer = 0 To numSections - 1
                        Dim sec As Integer = sections + s * 40
                        Dim vsize As Integer = BitConverter.ToInt32(buf, sec + 8)
                        Dim va As Integer = BitConverter.ToInt32(buf, sec + 12)
                        Dim rawSize As Integer = BitConverter.ToInt32(buf, sec + 16)
                        Dim rawPtr As Integer = BitConverter.ToInt32(buf, sec + 20)
                        If rva >= va AndAlso rva < va + Math.Max(vsize, rawSize) Then
                            Return If(rva - va < rawSize, rva - va + rawPtr, -1)
                        End If
                    Next
                    Return -1
                End Function
            Dim exp As Integer = toOffset(BitConverter.ToInt32(buf, dataDirs))
            If exp < 0 Then Return -1
            Dim numNames As Integer = BitConverter.ToInt32(buf, exp + 24)
            Dim funcs As Integer = toOffset(BitConverter.ToInt32(buf, exp + 28))
            Dim names As Integer = toOffset(BitConverter.ToInt32(buf, exp + 32))
            Dim ords As Integer = toOffset(BitConverter.ToInt32(buf, exp + 36))
            For i As Integer = 0 To numNames - 1
                Dim p As Integer = toOffset(BitConverter.ToInt32(buf, names + 4 * i))
                If p < 0 Then Continue For
                Dim e As Integer = Array.IndexOf(buf, CByte(0), p)
                If e < 0 Then Continue For
                If Encoding.ASCII.GetString(buf, p, e - p) = name Then
                    Dim ord As Integer = BitConverter.ToUInt16(buf, ords + 2 * i)
                    Return toOffset(BitConverter.ToInt32(buf, funcs + 4 * ord))
                End If
            Next
        Catch ex As ArgumentException
        Catch ex As IndexOutOfRangeException
        End Try
        Return -1
    End Function

End Module
