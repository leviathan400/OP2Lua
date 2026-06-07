' OP2 Tools - About Form
' Reusable About dialog for OP2 VB.NET projects
'
' To use in your project:
' 1. Copy fAbout.vb and fAbout.Designer.vb to your project
' 2. Add a banner image resource named "banner01_620x100" (620x100 pixels)
' 3. Set your application icon resource
' 4. Call: fAbout.ShowAbout("App Name", "1.0.0", "0001", Me)
'    Or simply: Dim about As New fAbout() : about.ShowDialog(Me)

Imports System.Runtime.CompilerServices
Imports System.Runtime.Serialization
Imports System.Runtime.Serialization.Formatters
Imports System.Xml
Imports Microsoft.SqlServer.Server
Imports OP2UtilityDotNet.Archive

Public Class fAbout

    ' Customizable properties - set these before showing the form
    ' or use the shared ShowAbout method
    Public Property ApplicationTitle As String = "OP2 Tool"
    Public Property ApplicationVersion As String = "1.0.0"
    Public Property ApplicationBuild As String = "0001"
    Public Property ApplicationDescription As String = "An Outpost 2 tool."
    Public Property ApplicationCopyright As String = "Outpost Universe © 2025"
    Public Property ApplicationWebsite As String = "https://outpost2.net"

    ''' <summary>
    ''' Convenience method to show the About dialog with custom values
    ''' </summary>
    Public Shared Sub ShowAbout(title As String, version As String, build As String, owner As Form,
                                 Optional description As String = Nothing,
                                 Optional copyright As String = Nothing,
                                 Optional website As String = Nothing)
        Using about As New fAbout()
            about.ApplicationTitle = title
            about.ApplicationVersion = version
            about.ApplicationBuild = build

            If description IsNot Nothing Then about.ApplicationDescription = description
            If copyright IsNot Nothing Then about.ApplicationCopyright = copyright
            If website IsNot Nothing Then about.ApplicationWebsite = website

            about.ShowDialog(owner)
        End Using
    End Sub

    Private Sub fAbout_Load(sender As Object, e As EventArgs) Handles MyBase.Load
        ' Set form properties
        Me.Text = "About " & ApplicationTitle

        ' Try to set icon from resources (customize resource name as needed)
        Try
            Me.Icon = My.Resources.AppIcon
        Catch
            ' Icon resource not found, use default
        End Try

        ' Set banner image
        Try
            panelBanner.BackgroundImage = My.Resources.banner02_620x100
            panelBanner.BackgroundImageLayout = ImageLayout.Zoom
        Catch
            ' Banner resource not found, keep default color
        End Try

        ' Set labels
        lblAppName.Text = ApplicationTitle
        lblVersion.Text = "Version " & ApplicationVersion & " (Build " & ApplicationBuild & ")"
        lblDescription.Text = ApplicationDescription
        lblCopyright.Text = ApplicationCopyright

        ' Set website link
        If Not String.IsNullOrWhiteSpace(ApplicationWebsite) Then
            linkWebsite.Text = ApplicationWebsite
            linkWebsite.Visible = True
        Else
            linkWebsite.Visible = False
        End If

        ' Set focus to OK button so Enter closes the form
        Me.ActiveControl = btnOK

        Debug.WriteLine("About Form Loaded")
    End Sub

    Private Sub linkWebsite_LinkClicked(sender As Object, e As LinkLabelLinkClickedEventArgs) Handles linkWebsite.LinkClicked
        Try
            Process.Start(New ProcessStartInfo(ApplicationWebsite) With {.UseShellExecute = True})
        Catch ex As Exception
            ' Failed to open URL
            MessageBox.Show("Could not open the website: " & ex.Message, "Error",
                          MessageBoxButtons.OK, MessageBoxIcon.Error)
        End Try
    End Sub

    Private Sub btnOK_Click(sender As Object, e As EventArgs) Handles btnOK.Click
        Me.Close()
    End Sub

End Class