// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayDialogs.h"

#include "pcsx2/Netplay.h"

#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace NetplayDialogs
{
	// How often the Netplay window looks at the session.
	static constexpr int ROOM_POLL_MS = 100;

	std::optional<std::string> AskForRoomCode(QWidget* parent)
	{
		QDialog dialog(parent);
		dialog.setWindowTitle(QDialog::tr("Join a Netplay Match"));

		QVBoxLayout* layout = new QVBoxLayout(&dialog);
		QLabel* explanation = new QLabel(QDialog::tr("Joins the room whose code the host sent you, on the game selected "
													 "in the game list; you are player 2. Both players need the same "
													 "NA228 ISO and BIOS."),
			&dialog);
		explanation->setWordWrap(true);
		layout->addWidget(explanation);

		QFormLayout* form = new QFormLayout();
		layout->addLayout(form);
		QLineEdit* code = new QLineEdit(&dialog);
		code->setMaxLength(6);
		code->setPlaceholderText(QStringLiteral("ABC234"));
		form->addRow(QDialog::tr("Room code:"), code);

		QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		buttons->button(QDialogButtonBox::Ok)->setText(QDialog::tr("Join"));
		layout->addWidget(buttons);
		QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

		const auto update = [code, buttons]() {
			const QString upper = code->text().trimmed().toUpper();
			if (upper != code->text())
				code->setText(upper);
			buttons->button(QDialogButtonBox::Ok)->setEnabled(Netplay::IsRoomCode(upper.toStdString()));
		};
		QObject::connect(code, &QLineEdit::textEdited, &dialog, update);
		update();

		if (dialog.exec() != QDialog::Accepted)
			return std::nullopt;
		return code->text().toStdString();
	}

	QDialog* ShowRoom(QWidget* parent, bool host, std::function<void()> cancel)
	{
		QDialog* dialog = new QDialog(parent);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(host ? QDialog::tr("Hosting a Netplay Match") : QDialog::tr("Joining a Netplay Match"));

		QVBoxLayout* layout = new QVBoxLayout(dialog);
		QLabel* code = new QLabel(dialog);
		QFont font = code->font();
		font.setPointSizeF(font.pointSizeF() * 2.5);
		font.setBold(true);
		code->setFont(font);
		code->setAlignment(Qt::AlignCenter);
		code->setTextInteractionFlags(Qt::TextSelectableByMouse);
		code->setVisible(host);
		layout->addWidget(code);
		QLabel* status = new QLabel(dialog);
		status->setWordWrap(true);
		layout->addWidget(status);
		QLabel* timing = new QLabel(dialog);
		layout->addWidget(timing);

		QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
		QPushButton* copy = nullptr;
		if (host)
		{
			copy = buttons->addButton(QDialog::tr("Copy Code"), QDialogButtonBox::ActionRole);
			copy->setEnabled(false);
			QObject::connect(copy, &QPushButton::clicked, dialog, [code]() { QGuiApplication::clipboard()->setText(code->text()); });
		}
		layout->addWidget(buttons);
		// Cancel, Close and the window's own close button reject the window; the boot accepts it.
		std::shared_ptr<bool> ended = std::make_shared<bool>(false);
		QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
		QObject::connect(dialog, &QDialog::rejected, dialog, [cancel, ended]() {
			if (!*ended)
				cancel();
		});

		QTimer* timer = new QTimer(dialog);
		const auto update = [timer, code, status, timing, copy, buttons, ended]() {
			const Netplay::RoomState room = Netplay::GetRoomState();
			if (!room.code.empty() && code->text().isEmpty())
			{
				code->setText(QString::fromStdString(room.code));
				if (copy)
					copy->setEnabled(true);
			}
			if (room.input_delay != 0)
			{
				timing->setText(room.ping_ms != 0 ?
									QDialog::tr("Ping %1 ms, input delay %2 frames.").arg(room.ping_ms).arg(room.input_delay) :
									QDialog::tr("Input delay %1 frames.").arg(room.input_delay));
			}
			if (!room.end_reason.empty())
			{
				status->setText(QDialog::tr("Netplay ended: %1.").arg(QString::fromStdString(room.end_reason)));
				buttons->button(QDialogButtonBox::Cancel)->setText(QDialog::tr("Close"));
				*ended = true;
				timer->stop();
			}
			else
			{
				status->setText(QString::fromStdString(room.status));
			}
		};
		QObject::connect(timer, &QTimer::timeout, dialog, update);
		update();
		timer->start(ROOM_POLL_MS);
		dialog->show();
		return dialog;
	}
} // namespace NetplayDialogs
